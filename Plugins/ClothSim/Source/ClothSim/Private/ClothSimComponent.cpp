// Copyright Epic Games, Inc. All Rights Reserved.

#include "ClothSimComponent.h"
#include "ClothSimResources.h"
#include "ClothMeshSceneProxy.h"
#include "ClothSceneViewExtension.h"

#include "DrawDebugHelpers.h"
#include "DynamicMeshBuilder.h"            // FDynamicMeshVertex
#include "Engine/Engine.h"                 // GEngine on-screen debug
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "RenderingThread.h"

UClothSimComponent::UClothSimComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	CastShadow = true;
	bUseAsOccluder = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

//////////////////////////////////////////////////////////////////////////
// Setup
//////////////////////////////////////////////////////////////////////////

void UClothSimComponent::BeginPlay()
{
	Super::BeginPlay();

	// Register the view extension that snapshots the Global Distance Field (M6).
	ClothGDF::EnsureRegistered();

	InitializeSimulation();

	// We now have geometry: rebuild bounds and recreate the (previously empty) proxy.
	UpdateBounds();
	MarkRenderStateDirty();
}

void UClothSimComponent::BuildTopology()
{
	Triangles.Reset();
	UV0.Reset();
	UV0.SetNumUninitialized(NumParticles);

	for (int32 Y = 0; Y < GridHeight; ++Y)
	{
		for (int32 X = 0; X < GridWidth; ++X)
		{
			UV0[Y * GridWidth + X] = FVector2f(
				(float)X / (float)(GridWidth - 1),
				(float)Y / (float)(GridHeight - 1));
		}
	}

	Triangles.Reserve((GridWidth - 1) * (GridHeight - 1) * 6);
	for (int32 Y = 0; Y < GridHeight - 1; ++Y)
	{
		for (int32 X = 0; X < GridWidth - 1; ++X)
		{
			const uint32 I00 = Y * GridWidth + X;
			const uint32 I10 = Y * GridWidth + (X + 1);
			const uint32 I01 = (Y + 1) * GridWidth + X;
			const uint32 I11 = (Y + 1) * GridWidth + (X + 1);

			Triangles.Add(I00); Triangles.Add(I01); Triangles.Add(I10);
			Triangles.Add(I10); Triangles.Add(I01); Triangles.Add(I11);
		}
	}
}

void UClothSimComponent::BuildConstraints(
	TArray<FGPUConstraint>& OutConstraints,
	TArray<FClothColorRange>& OutColorRanges) const
{
	OutConstraints.Reset();
	OutColorRanges.Reset();

	// --- 1. Gather edges (structural + shear + optional bending) -------------
	// Each edge stored once. RestLength from Spacing; StiffScale is the relative
	// stiffness applied on top of the global Stiffness uniform in the shader.
	const float RestStruct = Spacing;
	const float RestShear  = Spacing * FMath::Sqrt(2.0f);
	const float RestBend   = Spacing * 2.0f;

	TArray<FGPUConstraint> Edges;
	Edges.Reserve(NumParticles * 6);

	auto AddEdge = [&](int32 Ax, int32 Ay, int32 Bx, int32 By, float Rest, float StiffScale)
	{
		const uint32 A = (uint32)(Ay * GridWidth + Ax);
		const uint32 B = (uint32)(By * GridWidth + Bx);
		Edges.Add(FGPUConstraint{ A, B, Rest, StiffScale });
	};

	for (int32 Y = 0; Y < GridHeight; ++Y)
	{
		for (int32 X = 0; X < GridWidth; ++X)
		{
			// Structural: right + down (each edge added once).
			if (X + 1 < GridWidth)  AddEdge(X, Y, X + 1, Y,     RestStruct, 1.0f);
			if (Y + 1 < GridHeight) AddEdge(X, Y, X,     Y + 1, RestStruct, 1.0f);

			// Shear: both diagonals of the cell with this particle as its top-left.
			if (X + 1 < GridWidth && Y + 1 < GridHeight)
			{
				AddEdge(X,     Y, X + 1, Y + 1, RestShear, 1.0f); // ↘
				AddEdge(X + 1, Y, X,     Y + 1, RestShear, 1.0f); // ↙
			}

			// Bending: 2-away neighbour, right + down (softer).
			if (bUseBending)
			{
				if (X + 2 < GridWidth)  AddEdge(X, Y, X + 2, Y,     RestBend, BendStiffness);
				if (Y + 2 < GridHeight) AddEdge(X, Y, X,     Y + 2, RestBend, BendStiffness);
			}
		}
	}

	if (Edges.Num() == 0)
	{
		return;
	}

	// --- 2. Greedy graph coloring -------------------------------------------
	// Two constraints conflict if they share a particle. We assign each edge the
	// smallest color not yet used by either of its endpoints, so within a color no
	// particle is touched twice -> the GPU can project a whole color in parallel
	// with no data races. TSet<>-per-particle keeps the "colors used here" lookup O(1).
	TArray<int32> EdgeColor;
	EdgeColor.SetNumUninitialized(Edges.Num());

	TArray<TSet<int32>> UsedColorsAt;
	UsedColorsAt.SetNum(NumParticles);

	int32 NumColors = 0;
	for (int32 e = 0; e < Edges.Num(); ++e)
	{
		const int32 A = (int32)Edges[e].IndexA;
		const int32 B = (int32)Edges[e].IndexB;

		int32 Color = 0;
		while (UsedColorsAt[A].Contains(Color) || UsedColorsAt[B].Contains(Color))
		{
			++Color;
		}

		EdgeColor[e] = Color;
		UsedColorsAt[A].Add(Color);
		UsedColorsAt[B].Add(Color);
		NumColors = FMath::Max(NumColors, Color + 1);
	}

	// --- 3. Bucket edges into a color-sorted buffer + ranges ----------------
	TArray<int32> CountPerColor;
	CountPerColor.Init(0, NumColors);
	for (int32 e = 0; e < Edges.Num(); ++e)
	{
		++CountPerColor[EdgeColor[e]];
	}

	OutColorRanges.SetNum(NumColors);
	int32 Running = 0;
	for (int32 c = 0; c < NumColors; ++c)
	{
		OutColorRanges[c].Start = Running;
		OutColorRanges[c].Count = CountPerColor[c];
		Running += CountPerColor[c];
	}

	// Stable scatter into the sorted positions using a per-color write cursor.
	OutConstraints.SetNumUninitialized(Edges.Num());
	TArray<int32> Cursor;
	Cursor.SetNumUninitialized(NumColors);
	for (int32 c = 0; c < NumColors; ++c)
	{
		Cursor[c] = OutColorRanges[c].Start;
	}
	for (int32 e = 0; e < Edges.Num(); ++e)
	{
		const int32 c = EdgeColor[e];
		OutConstraints[Cursor[c]++] = Edges[e];
	}
}

void UClothSimComponent::InitializeSimulation()
{
	GridWidth  = FMath::Clamp(GridWidth, 2, 256);
	GridHeight = FMath::Clamp(GridHeight, 2, 256);
	NumParticles = GridWidth * GridHeight;

	BuildTopology();

	TArray<FVector3f> Positions;   // world space (sim runs in world space)
	TArray<FVector3f> Velocities;
	TArray<float>     InvMasses;
	Positions.Reserve(NumParticles);
	Velocities.Reserve(NumParticles);
	InvMasses.Reserve(NumParticles);
	InitialLocalPositions.Reset();
	InitialLocalPositions.Reserve(NumParticles);

	const FTransform& Xform = GetComponentTransform();

	// Grid in the component's local X-Z plane: width along +X, height down -Z.
	for (int32 Y = 0; Y < GridHeight; ++Y)
	{
		for (int32 X = 0; X < GridWidth; ++X)
		{
			const FVector Local(X * Spacing, 0.0f, -Y * Spacing);
			InitialLocalPositions.Add(FVector3f(Local));

			Positions.Add(FVector3f(Xform.TransformPosition(Local)));
			Velocities.Add(FVector3f::ZeroVector);

			const bool bPinnedCorner = bPinTopCorners && (Y == 0) && (X == 0 || X == GridWidth - 1);
			const bool bPinnedEdge   = bPinTopEdge && (Y == 0);
			InvMasses.Add((bPinnedCorner || bPinnedEdge) ? 0.0f : 1.0f);
		}
	}

	// Generous local bounds so the cloth isn't frustum-culled as it sags/blows.
	FBox Box(ForceInit);
	for (const FVector3f& P : InitialLocalPositions)
	{
		Box += FVector(P);
	}
	const float Margin = (GridWidth + GridHeight) * Spacing;
	Box = Box.ExpandBy(Margin);
	LocalBounds = FBoxSphereBounds(Box);

	// Explicit constraints + graph coloring for the Gauss-Seidel solver (M7).
	TArray<FGPUConstraint>   Constraints;
	TArray<FClothColorRange> ColorRanges;
	BuildConstraints(Constraints, ColorRanges);
	NumConstraintsBuilt = Constraints.Num();
	NumColorsBuilt      = ColorRanges.Num();

	RenderResources = MakeShared<FClothRenderResources>();

	TSharedPtr<FClothRenderResources> Resources = RenderResources;
	ENQUEUE_RENDER_COMMAND(ClothSimInit)(
		[Resources, Positions = MoveTemp(Positions), Velocities = MoveTemp(Velocities), InvMasses = MoveTemp(InvMasses),
		 Constraints = MoveTemp(Constraints), ColorRanges = MoveTemp(ColorRanges)]
		(FRHICommandListImmediate& RHICmdList)
		{
			ClothSimCompute::InitResources_RenderThread(RHICmdList, Resources, Positions, Velocities, InvMasses, Constraints, ColorRanges);
		});
}

//////////////////////////////////////////////////////////////////////////
// Primitive / mesh interface
//////////////////////////////////////////////////////////////////////////

FBoxSphereBounds UClothSimComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (NumParticles <= 0)
	{
		return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(1.0f), 1.0f);
	}
	return LocalBounds.TransformBy(LocalToWorld);
}

UMaterialInterface* UClothSimComponent::GetMaterial(int32 ElementIndex) const
{
	return ClothMaterial;
}

void UClothSimComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (ClothMaterial)
	{
		OutMaterials.Add(ClothMaterial);
	}
}

FPrimitiveSceneProxy* UClothSimComponent::CreateSceneProxy()
{
	if (NumParticles <= 0 || Triangles.Num() == 0 || InitialLocalPositions.Num() != NumParticles)
	{
		return nullptr; // not initialised yet (e.g. editor, before BeginPlay)
	}

	// Seed the proxy with the initial (flat) mesh; normals computed from the grid.
	TArray<FVector3f> SeedNormals, SeedTangents;
	ComputeGridNormalsTangents(InitialLocalPositions, SeedNormals, SeedTangents);

	TArray<FDynamicMeshVertex> Vertices;
	Vertices.SetNumUninitialized(NumParticles);
	for (int32 i = 0; i < NumParticles; ++i)
	{
		FDynamicMeshVertex& V = Vertices[i];
		V.Position = InitialLocalPositions[i];
		V.TextureCoordinate[0] = UV0[i];
		V.TangentX = FVector3f(SeedTangents[i]);
		V.TangentZ = FVector3f(SeedNormals[i]);
		V.Color = FColor::White;
	}

	return new FClothMeshSceneProxy(this, Vertices, Triangles, ClothMaterial);
}

//////////////////////////////////////////////////////////////////////////
// Per-frame
//////////////////////////////////////////////////////////////////////////

void UClothSimComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!RenderResources.IsValid() || NumParticles <= 0)
	{
		return;
	}

	FClothSimParams Params;
	Params.NumParticles     = NumParticles;
	Params.GridWidth        = GridWidth;
	Params.GridHeight       = GridHeight;
	Params.DeltaTime        = FixedTimeStep;
	Params.Gravity          = FVector3f(Gravity);
	Params.Damping          = Damping;
	Params.Substeps         = Substeps;
	Params.SolverIterations = SolverIterations;
	Params.Stiffness        = Stiffness;
	Params.RestStructural   = Spacing;
	Params.RestShear        = Spacing * FMath::Sqrt(2.0f);
	Params.bUseGaussSeidel  = (SolverMode == EClothSolverMode::GaussSeidel);

	// Wind (M4).
	Params.WindVelocity   = FVector3f(WindDirection.GetSafeNormal() * WindStrength);
	Params.WindDrag       = WindDrag;
	Params.WindTurbulence = WindTurbulence;
	Params.TimeSeconds    = GetWorld() ? (float)GetWorld()->GetTimeSeconds() : 0.0f;

	// Collision (M5/M6).
	Params.Friction                   = Friction;
	Params.bUseDistanceFieldCollision = bUseDistanceFieldCollision;
	Params.DFThickness                = DistanceFieldThickness;

	// Build world-space colliders from the authored slots.
	const FTransform& Xform = GetComponentTransform();
	Params.Colliders.Reserve(Colliders.Num());
	for (const FClothCollider& C : Colliders)
	{
		FGPUCollider G;
		G.Radius   = C.Radius;
		G.Friction = Friction;

		if (C.Type == EClothColliderType::Sphere)
		{
			const FVector World = Xform.TransformPosition(C.Center);
			G.A = FVector3f(World);
			G.B = G.A; // degenerate capsule == sphere
		}
		else // Capsule: endpoints = center ± (axis * halfHeight), in local then to world
		{
			const FVector Axis = C.Rotation.RotateVector(FVector::UpVector);
			const FVector LocalA = C.Center + Axis * C.HalfHeight;
			const FVector LocalB = C.Center - Axis * C.HalfHeight;
			G.A = FVector3f(Xform.TransformPosition(LocalA));
			G.B = FVector3f(Xform.TransformPosition(LocalB));
		}
		Params.Colliders.Add(G);
	}

	// Fixed-timestep accumulator (frame-rate independent, see header).
	TimeAccumulator += DeltaTime;
	TimeAccumulator = FMath::Min(TimeAccumulator, FixedTimeStep * MaxStepsPerFrame);

	int32 Steps = 0;
	while (TimeAccumulator >= FixedTimeStep && Steps < MaxStepsPerFrame)
	{
		TSharedPtr<FClothRenderResources> Resources = RenderResources;
		ENQUEUE_RENDER_COMMAND(ClothSimDispatch)(
			[Resources, Params](FRHICommandListImmediate& RHICmdList)
			{
				ClothSimCompute::Dispatch_RenderThread(RHICmdList, Resources, Params);
			});

		TimeAccumulator -= FixedTimeStep;
		++Steps;
	}

	UpdateMeshFromSimulation();

	if (bDrawDebugPoints)
	{
		DrawDebug();
	}
	if (bDrawColliders)
	{
		DrawColliders();
	}

	// M8 stats: solver config + work-per-substep, for the Jacobi-vs-Gauss-Seidel comparison.
	if (bShowStats && GEngine)
	{
		const bool bGS = (SolverMode == EClothSolverMode::GaussSeidel);
		// Solve dispatches issued per substep (the headline cost difference).
		const int32 SolveDispatches = bGS ? (SolverIterations * NumColorsBuilt) : SolverIterations;

		GEngine->AddOnScreenDebugMessage(
			(uint64)(UPTRINT)this + 1, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("ClothSim [%s]  particles=%d  substeps=%d  iters=%d"),
				bGS ? TEXT("Gauss-Seidel") : TEXT("Jacobi"),
				NumParticles, Substeps, SolverIterations));

		GEngine->AddOnScreenDebugMessage(
			(uint64)(UPTRINT)this + 2, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("  constraints=%d  colors=%d  bending=%d  solve dispatches/substep=%d"),
				NumConstraintsBuilt, NumColorsBuilt, bUseBending ? 1 : 0, SolveDispatches));
	}

	// M6 diagnostic: report whether the Global Distance Field snapshot is reaching us.
	if (bUseDistanceFieldCollision && GEngine)
	{
		const FClothGDFCache& Cache = ClothGDF::Get();
		GEngine->AddOnScreenDebugMessage(
			(uint64)(UPTRINT)this, 0.0f,
			Cache.bValid ? FColor::Green : FColor::Red,
			FString::Printf(TEXT("ClothSim GDF: valid=%d  clipmaps=%d"),
				Cache.bValid ? 1 : 0,
				Cache.bValid ? Cache.Data.NumGlobalSDFClipmaps : 0));
	}
}

void UClothSimComponent::DrawColliders()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FTransform& Xform = GetComponentTransform();
	const FColor Color = FColor::Yellow;

	for (const FClothCollider& C : Colliders)
	{
		if (C.Type == EClothColliderType::Sphere)
		{
			const FVector Center = Xform.TransformPosition(C.Center);
			DrawDebugSphere(World, Center, C.Radius, 16, Color, false, -1.0f, SDPG_World, 0.5f);
		}
		else // Capsule
		{
			const FVector Center = Xform.TransformPosition(C.Center);
			// UE's DrawDebugCapsule half-height is centre->tip (includes the hemisphere),
			// while our HalfHeight is the segment half-length, so add the radius.
			const FQuat Rot = (Xform.GetRotation() * C.Rotation.Quaternion());
			DrawDebugCapsule(World, Center, C.HalfHeight + C.Radius, C.Radius, Rot, Color, false, -1.0f, SDPG_World, 0.5f);
		}
	}
}

void UClothSimComponent::ComputeGridNormalsTangents(
	const TArray<FVector3f>& InPositions,
	TArray<FVector3f>& OutNormals,
	TArray<FVector3f>& OutTangents) const
{
	OutNormals.Init(FVector3f::ZeroVector, NumParticles);
	OutTangents.Init(FVector3f::ZeroVector, NumParticles);

	// Area-weighted face normals accumulated to each vertex.
	for (int32 t = 0; t < Triangles.Num(); t += 3)
	{
		const uint32 I0 = Triangles[t];
		const uint32 I1 = Triangles[t + 1];
		const uint32 I2 = Triangles[t + 2];

		const FVector3f E1 = InPositions[I1] - InPositions[I0];
		const FVector3f E2 = InPositions[I2] - InPositions[I0];
		const FVector3f FaceN = FVector3f::CrossProduct(E1, E2);

		OutNormals[I0] += FaceN;
		OutNormals[I1] += FaceN;
		OutNormals[I2] += FaceN;
	}

	for (int32 Y = 0; Y < GridHeight; ++Y)
	{
		for (int32 X = 0; X < GridWidth; ++X)
		{
			const int32 i = Y * GridWidth + X;

			FVector3f N = OutNormals[i];
			N = N.GetSafeNormal(SMALL_NUMBER, FVector3f(0.0f, 1.0f, 0.0f));
			OutNormals[i] = N;

			// Tangent along +X (u direction) using neighbours, made orthogonal to N.
			const int32 XPrev = FMath::Max(X - 1, 0);
			const int32 XNext = FMath::Min(X + 1, GridWidth - 1);
			FVector3f T = InPositions[Y * GridWidth + XNext] - InPositions[Y * GridWidth + XPrev];
			T = (T - N * FVector3f::DotProduct(N, T)).GetSafeNormal(SMALL_NUMBER, FVector3f(1.0f, 0.0f, 0.0f));
			OutTangents[i] = T;
		}
	}
}

void UClothSimComponent::ComputeStrainColors(
	const TArray<FVector3f>& InPositions, TArray<FColor>& OutColors) const
{
	OutColors.SetNumUninitialized(NumParticles);

	const float Rest = Spacing;
	const float InvScale = 1.0f / FMath::Max(StrainScale, 0.01f);

	// Color ramp: green at rest, lerp to red when stretched, to blue when compressed.
	const FLinearColor Rest_C(0.0f, 1.0f, 0.0f);
	const FLinearColor Stretch_C(1.0f, 0.0f, 0.0f);
	const FLinearColor Compress_C(0.0f, 0.35f, 1.0f);

	for (int32 Y = 0; Y < GridHeight; ++Y)
	{
		for (int32 X = 0; X < GridWidth; ++X)
		{
			const int32 i = Y * GridWidth + X;

			// Average signed strain over the existing structural (cardinal) neighbours.
			float StrainSum = 0.0f;
			int32 Count = 0;
			auto Accumulate = [&](int32 Nx, int32 Ny)
			{
				if (Nx < 0 || Ny < 0 || Nx >= GridWidth || Ny >= GridHeight)
				{
					return;
				}
				const int32 j = Ny * GridWidth + Nx;
				const float Len = (InPositions[i] - InPositions[j]).Size();
				StrainSum += (Len - Rest) / Rest;
				++Count;
			};
			Accumulate(X - 1, Y);
			Accumulate(X + 1, Y);
			Accumulate(X, Y - 1);
			Accumulate(X, Y + 1);

			const float Strain = (Count > 0) ? (StrainSum / Count) : 0.0f;
			const float T = FMath::Clamp(Strain * InvScale, -1.0f, 1.0f);

			const FLinearColor C = (T >= 0.0f)
				? FMath::Lerp(Rest_C, Stretch_C, T)
				: FMath::Lerp(Rest_C, Compress_C, -T);
			OutColors[i] = C.ToFColor(/*bSRGB*/ false);
		}
	}
}

void UClothSimComponent::UpdateMeshFromSimulation()
{
	FClothMeshSceneProxy* Proxy = static_cast<FClothMeshSceneProxy*>(SceneProxy);
	if (!Proxy || !RenderResources.IsValid())
	{
		return;
	}

	// Copy the latest world-space positions out of the readback buffer.
	{
		FScopeLock Lock(&RenderResources->DebugCopyCS);
		if (!RenderResources->bHasDebugData || RenderResources->DebugPositions.Num() != NumParticles)
		{
			return; // readback not ready yet
		}

		const FTransform WorldToLocal = GetComponentTransform().Inverse();
		LocalPositions.SetNumUninitialized(NumParticles);
		for (int32 i = 0; i < NumParticles; ++i)
		{
			const FVector World(RenderResources->DebugPositions[i]);
			LocalPositions[i] = FVector3f(WorldToLocal.TransformPosition(World));
		}
	}

	ComputeGridNormalsTangents(LocalPositions, LocalNormals, LocalTangents);

	// Strain colors (M8). Empty array => proxy leaves vertex colors unchanged.
	if (bVisualizeStrain)
	{
		ComputeStrainColors(LocalPositions, LocalColors);
	}
	else
	{
		LocalColors.Reset();
	}

	// Hand the new vertex data to the proxy on the render thread.
	ENQUEUE_RENDER_COMMAND(ClothMeshUpdate)(
		[Proxy, Positions = LocalPositions, Normals = LocalNormals, Tangents = LocalTangents, Colors = LocalColors]
		(FRHICommandListImmediate& RHICmdList)
		{
			Proxy->UpdateVertices_RenderThread(RHICmdList, Positions, Normals, Tangents, Colors);
		});
}

void UClothSimComponent::DrawDebug()
{
	UWorld* World = GetWorld();
	if (!World || !RenderResources.IsValid())
	{
		return;
	}

	FScopeLock Lock(&RenderResources->DebugCopyCS);
	if (!RenderResources->bHasDebugData)
	{
		return;
	}

	// Color by strain when visualizing (LocalColors is refreshed in UpdateMeshFromSimulation
	// just before this call), otherwise a flat cyan.
	const bool bUseStrain = bVisualizeStrain && LocalColors.Num() == RenderResources->DebugPositions.Num();

	for (int32 i = 0; i < RenderResources->DebugPositions.Num(); ++i)
	{
		const FColor Color = bUseStrain ? LocalColors[i] : FColor::Cyan;
		DrawDebugPoint(World, FVector(RenderResources->DebugPositions[i]), DebugPointSize, Color, false, -1.0f, SDPG_World);
	}
}

void UClothSimComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (RenderResources.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(ClothSimRelease)(
			[Resources = MoveTemp(RenderResources)](FRHICommandListImmediate&) mutable
			{
				Resources.Reset();
			});
	}

	Super::EndPlay(EndPlayReason);
}
