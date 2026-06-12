// Copyright Epic Games, Inc. All Rights Reserved.

#include "ClothSimComponent.h"
#include "ClothSimResources.h"
#include "ClothMeshSceneProxy.h"

#include "DrawDebugHelpers.h"
#include "DynamicMeshBuilder.h"            // FDynamicMeshVertex
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

			const bool bPinned = bPinTopCorners && (Y == 0) && (X == 0 || X == GridWidth - 1);
			InvMasses.Add(bPinned ? 0.0f : 1.0f);
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

	RenderResources = MakeShared<FClothRenderResources>();

	TSharedPtr<FClothRenderResources> Resources = RenderResources;
	ENQUEUE_RENDER_COMMAND(ClothSimInit)(
		[Resources, Positions = MoveTemp(Positions), Velocities = MoveTemp(Velocities), InvMasses = MoveTemp(InvMasses)]
		(FRHICommandListImmediate& RHICmdList)
		{
			ClothSimCompute::InitResources_RenderThread(RHICmdList, Resources, Positions, Velocities, InvMasses);
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

	// Wind (M4).
	Params.WindVelocity   = FVector3f(WindDirection.GetSafeNormal() * WindStrength);
	Params.WindDrag       = WindDrag;
	Params.WindTurbulence = WindTurbulence;
	Params.TimeSeconds    = GetWorld() ? (float)GetWorld()->GetTimeSeconds() : 0.0f;

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

	// Hand the new vertex data to the proxy on the render thread.
	ENQUEUE_RENDER_COMMAND(ClothMeshUpdate)(
		[Proxy, Positions = LocalPositions, Normals = LocalNormals, Tangents = LocalTangents]
		(FRHICommandListImmediate& RHICmdList)
		{
			Proxy->UpdateVertices_RenderThread(RHICmdList, Positions, Normals, Tangents);
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

	for (const FVector3f& P : RenderResources->DebugPositions)
	{
		DrawDebugPoint(World, FVector(P), DebugPointSize, FColor::Cyan, false, -1.0f, SDPG_World);
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
