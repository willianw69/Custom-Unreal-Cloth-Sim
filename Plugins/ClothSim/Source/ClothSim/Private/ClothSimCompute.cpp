// Copyright Epic Games, Inc. All Rights Reserved.

#include "ClothSimResources.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalDistanceFieldParameters.h"
#include "ClothSceneViewExtension.h"
#include "SceneView.h" // FViewUniformShaderParameters

// Must match [numthreads(...)] in every cloth .usf. Injected as a #define.
static constexpr uint32 kThreadGroupSize = 64;

// Self-collision hash grid: max particle indices stored per bucket. Injected as MAX_PER_CELL.
static constexpr uint32 kMaxPerCell = 16;

// Smallest prime >= N, for the spatial-hash table size (reduces modulo clustering).
static uint32 NextPrime(uint32 N)
{
	auto IsPrime = [](uint32 X) -> bool
	{
		if (X < 2) return false;
		if (X % 2 == 0) return X == 2;
		for (uint32 d = 3; d * d <= X; d += 2)
		{
			if (X % d == 0) return false;
		}
		return true;
	};
	N = FMath::Max(N, 3u);
	if (N % 2 == 0) ++N;
	while (!IsPrime(N)) N += 2;
	return N;
}

//////////////////////////////////////////////////////////////////////////
// Shader bindings: Predict -> SolveDistance (xN) -> Finalize
//////////////////////////////////////////////////////////////////////////

class FClothPredictCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClothPredictCS);
	SHADER_USE_PARAMETER_STRUCT(FClothPredictCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, Positions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, Velocities)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, PredictedPositions)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(uint32, GridWidth)
		SHADER_PARAMETER(uint32, GridHeight)
		SHADER_PARAMETER(float, SubDeltaTime)
		SHADER_PARAMETER(FVector3f, Gravity)
		SHADER_PARAMETER(float, Damping)
		SHADER_PARAMETER(FVector3f, WindVelocity)
		SHADER_PARAMETER(float, WindDrag)
		SHADER_PARAMETER(float, WindTurbulence)
		SHADER_PARAMETER(float, TimeSeconds)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

class FClothSolveDistanceCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClothSolveDistanceCS);
	SHADER_USE_PARAMETER_STRUCT(FClothSolveDistanceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, PredictedIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, PredictedOut)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(uint32, GridWidth)
		SHADER_PARAMETER(uint32, GridHeight)
		SHADER_PARAMETER(float, RestStructural)
		SHADER_PARAMETER(float, RestShear)
		SHADER_PARAMETER(float, Stiffness)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

// Graph-colored Gauss-Seidel distance solve: one thread per constraint, one dispatch
// per color, projecting both endpoints in place (M7).
class FClothSolveGaussSeidelCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClothSolveGaussSeidelCS);
	SHADER_USE_PARAMETER_STRUCT(FClothSolveGaussSeidelCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUConstraint>, Constraints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, Positions)
		SHADER_PARAMETER(uint32, ColorStart)
		SHADER_PARAMETER(uint32, ColorCount)
		SHADER_PARAMETER(float, Stiffness)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

// Self-collision broadphase build: bin particles into a spatial hash grid (M9).
class FClothBuildGridCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClothBuildGridCS);
	SHADER_USE_PARAMETER_STRUCT(FClothBuildGridCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, PredictedIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellParticles)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(uint32, TableSize)
		SHADER_PARAMETER(float, CellSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("MAX_PER_CELL"), kMaxPerCell);
	}
};

// Self-collision response: scan the 27 neighbour cells, repel close non-adjacent particles (M9).
class FClothSelfCollisionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClothSelfCollisionCS);
	SHADER_USE_PARAMETER_STRUCT(FClothSelfCollisionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, PredictedIn)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CellCounts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellParticles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, PredictedOut)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(uint32, GridWidth)
		SHADER_PARAMETER(uint32, GridHeight)
		SHADER_PARAMETER(uint32, TableSize)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(float, Thickness)
		SHADER_PARAMETER(float, SelfStiffness)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("MAX_PER_CELL"), kMaxPerCell);
	}
};

class FClothFinalizeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClothFinalizeCS);
	SHADER_USE_PARAMETER_STRUCT(FClothFinalizeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, PredictedPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, Velocities)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(float, InvSubDeltaTime)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

class FClothCollisionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClothCollisionCS);
	SHADER_USE_PARAMETER_STRUCT(FClothCollisionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, PredictedPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, PrevPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FGPUCollider>, Colliders)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(uint32, NumColliders)
		SHADER_PARAMETER(float, ContactOffset)
		SHADER_PARAMETER(uint32, EnableGround)
		SHADER_PARAMETER(float, GroundZ)
		SHADER_PARAMETER(float, GroundFriction)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

// Distance-field collision against arbitrary scene meshes (Global Distance Field).
class FClothCollisionDFCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FClothCollisionDFCS);
	SHADER_USE_PARAMETER_STRUCT(FClothCollisionDFCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, PredictedPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, PrevPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InvMasses)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(FVector3f, PreViewTranslation)
		SHADER_PARAMETER(float, Thickness)
		SHADER_PARAMETER(float, Friction)
		// The GDF inputs come from this struct; View is only needed to satisfy the
		// engine header's transitive ResolvedView references.
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FGlobalDistanceFieldParameters2, GlobalDistanceFieldParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), kThreadGroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FClothPredictCS,        "/ClothSim/Private/ClothPredict.usf",        "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClothCollisionCS,      "/ClothSim/Private/ClothCollision.usf",      "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClothCollisionDFCS,    "/ClothSim/Private/ClothCollisionDF.usf",    "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClothSolveDistanceCS,  "/ClothSim/Private/ClothSolveDistance.usf",  "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClothSolveGaussSeidelCS,"/ClothSim/Private/ClothSolveGaussSeidel.usf","MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClothBuildGridCS,      "/ClothSim/Private/ClothBuildGrid.usf",      "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClothSelfCollisionCS,  "/ClothSim/Private/ClothSelfCollision.usf",  "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClothFinalizeCS,       "/ClothSim/Private/ClothFinalize.usf",       "MainCS", SF_Compute);

//////////////////////////////////////////////////////////////////////////
// Resource lifetime
//////////////////////////////////////////////////////////////////////////

FClothRenderResources::FClothRenderResources() = default;

FClothRenderResources::~FClothRenderResources()
{
	PositionReadback.Reset();
}

//////////////////////////////////////////////////////////////////////////
// Init
//////////////////////////////////////////////////////////////////////////

void ClothSimCompute::InitResources_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	const TSharedPtr<FClothRenderResources>& Resources,
	const TArray<FVector3f>& InitialPositions,
	const TArray<FVector3f>& InitialVelocities,
	const TArray<float>& InitialInvMasses,
	const TArray<FGPUConstraint>& Constraints,
	const TArray<FClothColorRange>& ColorRanges)
{
	check(IsInRenderingThread());
	check(Resources.IsValid());

	const int32 Num = InitialPositions.Num();
	if (Num <= 0)
	{
		return;
	}
	Resources->NumParticles = Num;

	FRDGBuilder GraphBuilder(RHICmdList);

	FRDGBufferRef Positions = CreateStructuredBuffer(
		GraphBuilder, TEXT("Cloth.Positions"),
		sizeof(FVector3f), Num, InitialPositions.GetData(), sizeof(FVector3f) * Num);

	FRDGBufferRef Velocities = CreateStructuredBuffer(
		GraphBuilder, TEXT("Cloth.Velocities"),
		sizeof(FVector3f), Num, InitialVelocities.GetData(), sizeof(FVector3f) * Num);

	FRDGBufferRef InvMasses = CreateStructuredBuffer(
		GraphBuilder, TEXT("Cloth.InvMasses"),
		sizeof(float), Num, InitialInvMasses.GetData(), sizeof(float) * Num);

	GraphBuilder.QueueBufferExtraction(Positions, &Resources->PositionsBuffer);
	GraphBuilder.QueueBufferExtraction(Velocities, &Resources->VelocitiesBuffer);
	GraphBuilder.QueueBufferExtraction(InvMasses, &Resources->InvMassBuffer);

	// Constraint buffer for the Gauss-Seidel path (M7), sorted by color on the CPU.
	Resources->NumConstraints = Constraints.Num();
	Resources->ColorRanges = ColorRanges;
	if (Constraints.Num() > 0)
	{
		FRDGBufferRef ConstraintsBuf = CreateStructuredBuffer(
			GraphBuilder, TEXT("Cloth.Constraints"),
			sizeof(FGPUConstraint), Constraints.Num(),
			Constraints.GetData(), sizeof(FGPUConstraint) * Constraints.Num());
		GraphBuilder.QueueBufferExtraction(ConstraintsBuf, &Resources->ConstraintsBuffer);
	}

	GraphBuilder.Execute();

	Resources->PositionReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Cloth.PositionReadback"));
	Resources->bInitialized = true;
}

//////////////////////////////////////////////////////////////////////////
// Per-frame dispatch
//////////////////////////////////////////////////////////////////////////

void ClothSimCompute::Dispatch_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	const TSharedPtr<FClothRenderResources>& Resources,
	const FClothSimParams& Params)
{
	check(IsInRenderingThread());

	if (!Resources.IsValid() || !Resources->bInitialized || Resources->NumParticles <= 0)
	{
		return;
	}

	const int32 Num = Resources->NumParticles;

	// --- Consume LAST frame's readback (non-stalling) ----------------------
	if (Resources->PositionReadback->IsReady())
	{
		const uint32 NumBytes = sizeof(FVector3f) * Num;
		const FVector3f* Src = static_cast<const FVector3f*>(Resources->PositionReadback->Lock(NumBytes));
		if (Src)
		{
			FScopeLock Lock(&Resources->DebugCopyCS);
			Resources->DebugPositions.SetNumUninitialized(Num);
			FMemory::Memcpy(Resources->DebugPositions.GetData(), Src, NumBytes);
			Resources->bHasDebugData = true;
		}
		Resources->PositionReadback->Unlock();
	}

	FRDGBuilder GraphBuilder(RHICmdList);

	FRDGBufferRef Positions  = GraphBuilder.RegisterExternalBuffer(Resources->PositionsBuffer, TEXT("Cloth.Positions"));
	FRDGBufferRef Velocities = GraphBuilder.RegisterExternalBuffer(Resources->VelocitiesBuffer, TEXT("Cloth.Velocities"));
	FRDGBufferRef InvMasses  = GraphBuilder.RegisterExternalBuffer(Resources->InvMassBuffer, TEXT("Cloth.InvMasses"));
	FRDGBufferSRVRef InvMassesSRV = GraphBuilder.CreateSRV(InvMasses);

	// Two predicted-position workspace buffers, ping-ponged across solver iterations.
	// Transient: recomputed every frame from Positions/Velocities, so no need to persist.
	const FRDGBufferDesc PredictedDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), Num);
	FRDGBufferRef PredictedA = GraphBuilder.CreateBuffer(PredictedDesc, TEXT("Cloth.PredictedA"));
	FRDGBufferRef PredictedB = GraphBuilder.CreateBuffer(PredictedDesc, TEXT("Cloth.PredictedB"));

	const int32 Substeps   = FMath::Max(1, Params.Substeps);
	const int32 Iterations = FMath::Max(1, Params.SolverIterations);
	const float SubDt      = Params.DeltaTime / (float)Substeps;
	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(Num, kThreadGroupSize);

	// Colliders are constant within a frame; build the buffer once. The collision pass
	// also handles the optional ground plane, so we still need a (dummy) collider buffer
	// bound when there are no analytic colliders but the ground plane is enabled.
	const int32 NumColliders = Params.Colliders.Num();
	FRDGBufferSRVRef CollidersSRV = nullptr;
	if (NumColliders > 0)
	{
		FRDGBufferRef CollidersBuf = CreateStructuredBuffer(
			GraphBuilder, TEXT("Cloth.Colliders"),
			sizeof(FGPUCollider), NumColliders,
			Params.Colliders.GetData(), sizeof(FGPUCollider) * NumColliders);
		CollidersSRV = GraphBuilder.CreateSRV(CollidersBuf);
	}
	else if (Params.bGroundPlane)
	{
		const FGPUCollider Dummy; // never read: NumColliders is passed as 0
		FRDGBufferRef CollidersBuf = CreateStructuredBuffer(
			GraphBuilder, TEXT("Cloth.CollidersDummy"),
			sizeof(FGPUCollider), 1, &Dummy, sizeof(FGPUCollider));
		CollidersSRV = GraphBuilder.CreateSRV(CollidersBuf);
	}

	// Distance-field collision uses the GDF snapshot captured by the view extension.
	const FClothGDFCache& GDFCache = ClothGDF::Get();
	const bool bDoDistanceField = Params.bUseDistanceFieldCollision && GDFCache.bValid;

	// Gauss-Seidel path (M7): use the explicit, color-sorted constraint buffer when
	// requested and available. Falls back to the Jacobi gather otherwise.
	const bool bDoGaussSeidel = Params.bUseGaussSeidel
		&& Resources->NumConstraints > 0
		&& Resources->ConstraintsBuffer.IsValid()
		&& Resources->ColorRanges.Num() > 0;

	FRDGBufferSRVRef ConstraintsSRV = nullptr;
	if (bDoGaussSeidel)
	{
		FRDGBufferRef ConstraintsBuf = GraphBuilder.RegisterExternalBuffer(
			Resources->ConstraintsBuffer, TEXT("Cloth.Constraints"));
		ConstraintsSRV = GraphBuilder.CreateSRV(ConstraintsBuf);
	}

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FClothPredictCS>          PredictCS(ShaderMap);
	TShaderMapRef<FClothSolveDistanceCS>    SolveCS(ShaderMap);
	TShaderMapRef<FClothSolveGaussSeidelCS> GaussSeidelCS(ShaderMap);
	TShaderMapRef<FClothBuildGridCS>        BuildGridCS(ShaderMap);
	TShaderMapRef<FClothSelfCollisionCS>    SelfCollisionCS(ShaderMap);
	TShaderMapRef<FClothCollisionCS>        CollisionCS(ShaderMap);
	TShaderMapRef<FClothCollisionDFCS>      CollisionDFCS(ShaderMap);
	TShaderMapRef<FClothFinalizeCS>         FinalizeCS(ShaderMap);

	// Self-collision hash-grid sizing (M9). Constant for the frame.
	const uint32 SelfTableSize = NextPrime(2u * (uint32)Num);

	if (SubDt > 0.0f)
	{
		for (int32 Step = 0; Step < Substeps; ++Step)
		{
			// --- Predict: Positions/Velocities -> PredictedA ---
			{
				FClothPredictCS::FParameters* P = GraphBuilder.AllocParameters<FClothPredictCS::FParameters>();
				P->Positions          = GraphBuilder.CreateSRV(Positions);
				P->Velocities         = GraphBuilder.CreateSRV(Velocities);
				P->InvMasses          = InvMassesSRV;
				P->PredictedPositions = GraphBuilder.CreateUAV(PredictedA);
				P->NumParticles       = (uint32)Num;
				P->GridWidth          = (uint32)Params.GridWidth;
				P->GridHeight         = (uint32)Params.GridHeight;
				P->SubDeltaTime       = SubDt;
				P->Gravity            = Params.Gravity;
				P->Damping            = Params.Damping;
				P->WindVelocity       = Params.WindVelocity;
				P->WindDrag           = Params.WindDrag;
				P->WindTurbulence     = Params.WindTurbulence;
				P->TimeSeconds        = Params.TimeSeconds;

				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("ClothPredict (substep %d)", Step),
					PredictCS, P, GroupCount);
			}

			// --- Solve ---
			// Two strategies share the same Predict/Collision/Finalize scaffolding;
			// `In` ends up holding the latest solved positions either way.
			FRDGBufferRef In = PredictedA;

			if (bDoGaussSeidel)
			{
				// Graph-colored Gauss-Seidel: solve in place on PredictedA. Within a
				// color no two constraints share a particle (race-free UAV writes); each
				// color reads the previous one's results (RDG serializes the UAV), giving
				// true Gauss-Seidel propagation. No ping-pong needed.
				for (int32 Iter = 0; Iter < Iterations; ++Iter)
				{
					for (int32 Color = 0; Color < Resources->ColorRanges.Num(); ++Color)
					{
						const FClothColorRange& Range = Resources->ColorRanges[Color];
						if (Range.Count <= 0)
						{
							continue;
						}

						FClothSolveGaussSeidelCS::FParameters* P = GraphBuilder.AllocParameters<FClothSolveGaussSeidelCS::FParameters>();
						P->Constraints = ConstraintsSRV;
						P->InvMasses   = InvMassesSRV;
						P->Positions   = GraphBuilder.CreateUAV(In);
						P->ColorStart  = (uint32)Range.Start;
						P->ColorCount  = (uint32)Range.Count;
						P->Stiffness   = Params.Stiffness;

						FComputeShaderUtils::AddPass(GraphBuilder,
							RDG_EVENT_NAME("ClothSolveGaussSeidel (substep %d iter %d color %d)", Step, Iter, Color),
							GaussSeidelCS, P, FComputeShaderUtils::GetGroupCount(Range.Count, kThreadGroupSize));
					}
				}
				// `In` (= PredictedA) holds the solved positions.
			}
			else
			{
				// Jacobi per-particle gather (baseline): ping-pong PredictedA <-> PredictedB.
				FRDGBufferRef Out = PredictedB;
				for (int32 Iter = 0; Iter < Iterations; ++Iter)
				{
					FClothSolveDistanceCS::FParameters* P = GraphBuilder.AllocParameters<FClothSolveDistanceCS::FParameters>();
					P->PredictedIn     = GraphBuilder.CreateSRV(In);
					P->InvMasses       = InvMassesSRV;
					P->PredictedOut    = GraphBuilder.CreateUAV(Out);
					P->NumParticles    = (uint32)Num;
					P->GridWidth       = (uint32)Params.GridWidth;
					P->GridHeight      = (uint32)Params.GridHeight;
					P->RestStructural  = Params.RestStructural;
					P->RestShear       = Params.RestShear;
					P->Stiffness       = Params.Stiffness;

					FComputeShaderUtils::AddPass(GraphBuilder,
						RDG_EVENT_NAME("ClothSolveDistance (substep %d iter %d)", Step, Iter),
						SolveCS, P, GroupCount);

					Swap(In, Out);
				}
				// After the loop, `In` holds the latest solved positions.
			}

			// --- Self-collision: spatial hash broadphase + Jacobi repulsion (M9) ---
			// Runs before external colliders so a solid collider still gets the final say.
			// Ping-pongs In -> Other so the gather reads a clean snapshot (race-free).
			if (Params.bSelfCollision && Params.SelfThickness > 0.0f)
			{
				// Iterate to resolve deeper stacks (each iter rebuilds the grid from the
				// latest positions, then does one Jacobi repulsion pass). More iterations =
				// firmer separation but more cost.
				const int32 SelfIters = FMath::Max(1, Params.SelfCollisionIterations);
				for (int32 SIt = 0; SIt < SelfIters; ++SIt)
				{
					FRDGBufferRef Other = (In == PredictedA) ? PredictedB : PredictedA;

					// CellCounts is a TYPED uint buffer (clean ClearUAV + atomics); CellParticles
					// is a plain structured index list (never cleared, written by slot).
					const FRDGBufferDesc CountsDesc   = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SelfTableSize);
					const FRDGBufferDesc ParticleDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SelfTableSize * kMaxPerCell);
					FRDGBufferRef CellCounts    = GraphBuilder.CreateBuffer(CountsDesc,   TEXT("Cloth.SelfCellCounts"));
					FRDGBufferRef CellParticles = GraphBuilder.CreateBuffer(ParticleDesc, TEXT("Cloth.SelfCellParticles"));

					FRDGBufferUAVRef CellCountsUAV = GraphBuilder.CreateUAV(CellCounts, PF_R32_UINT);
					AddClearUAVPass(GraphBuilder, CellCountsUAV, 0u);

					// Build: bin particles into the hash grid.
					{
						FClothBuildGridCS::FParameters* P = GraphBuilder.AllocParameters<FClothBuildGridCS::FParameters>();
						P->PredictedIn   = GraphBuilder.CreateSRV(In);
						P->CellCounts    = CellCountsUAV;
						P->CellParticles = GraphBuilder.CreateUAV(CellParticles);
						P->NumParticles  = (uint32)Num;
						P->TableSize     = SelfTableSize;
						P->CellSize      = Params.SelfThickness;

						FComputeShaderUtils::AddPass(GraphBuilder,
							RDG_EVENT_NAME("ClothBuildGrid (substep %d iter %d)", Step, SIt),
							BuildGridCS, P, GroupCount);
					}

					// Respond: repel close non-adjacent particles, writing the other buffer.
					{
						FClothSelfCollisionCS::FParameters* P = GraphBuilder.AllocParameters<FClothSelfCollisionCS::FParameters>();
						P->PredictedIn   = GraphBuilder.CreateSRV(In);
						P->InvMasses     = InvMassesSRV;
						P->CellCounts    = GraphBuilder.CreateSRV(CellCounts, PF_R32_UINT);
						P->CellParticles = GraphBuilder.CreateSRV(CellParticles);
						P->PredictedOut  = GraphBuilder.CreateUAV(Other);
						P->NumParticles  = (uint32)Num;
						P->GridWidth     = (uint32)Params.GridWidth;
						P->GridHeight    = (uint32)Params.GridHeight;
						P->TableSize     = SelfTableSize;
						P->CellSize      = Params.SelfThickness;
						P->Thickness     = Params.SelfThickness;
						P->SelfStiffness = Params.SelfStiffness;

						FComputeShaderUtils::AddPass(GraphBuilder,
							RDG_EVENT_NAME("ClothSelfCollision (substep %d iter %d)", Step, SIt),
							SelfCollisionCS, P, GroupCount);
					}

					In = Other; // corrected positions feed the next iteration
				}
			}

			// --- Collision: project predicted positions out of colliders + friction ---
			if (CollidersSRV)
			{
				FClothCollisionCS::FParameters* P = GraphBuilder.AllocParameters<FClothCollisionCS::FParameters>();
				P->PredictedPositions = GraphBuilder.CreateUAV(In);    // in place; one thread per particle
				P->PrevPositions      = GraphBuilder.CreateSRV(Positions); // start-of-substep position
				P->InvMasses          = InvMassesSRV;
				P->Colliders          = CollidersSRV;
				P->NumParticles       = (uint32)Num;
				P->NumColliders       = (uint32)NumColliders;
				P->ContactOffset      = 1.0f; // cm skin so cloth rests just off the surface
				P->EnableGround       = Params.bGroundPlane ? 1u : 0u;
				P->GroundZ            = Params.GroundZ;
				P->GroundFriction     = Params.Friction;

				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("ClothCollision (substep %d)", Step),
					CollisionCS, P, GroupCount);
			}

			// --- Distance-field collision: project out of ANY scene mesh via the GDF ---
			if (bDoDistanceField)
			{
				FClothCollisionDFCS::FParameters* P = GraphBuilder.AllocParameters<FClothCollisionDFCS::FParameters>();
				P->PredictedPositions = GraphBuilder.CreateUAV(In);
				P->PrevPositions      = GraphBuilder.CreateSRV(Positions);
				P->InvMasses          = InvMassesSRV;
				P->NumParticles       = (uint32)Num;
				P->PreViewTranslation = GDFCache.PreViewTranslation;
				P->Thickness          = Params.DFThickness;
				P->Friction           = Params.Friction;
				P->View               = GDFCache.ViewUniformBuffer;
				P->GlobalDistanceFieldParameters = SetupGlobalDistanceFieldParameters_Minimal(GDFCache.Data);

				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("ClothCollisionDF (substep %d)", Step),
					CollisionDFCS, P, GroupCount);
			}

			// --- Finalize: derive velocity, commit positions ---
			{
				FClothFinalizeCS::FParameters* P = GraphBuilder.AllocParameters<FClothFinalizeCS::FParameters>();
				P->PredictedPositions = GraphBuilder.CreateSRV(In);
				P->InvMasses          = InvMassesSRV;
				P->Positions          = GraphBuilder.CreateUAV(Positions);
				P->Velocities         = GraphBuilder.CreateUAV(Velocities);
				P->NumParticles       = (uint32)Num;
				P->InvSubDeltaTime    = 1.0f / SubDt;

				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("ClothFinalize (substep %d)", Step),
					FinalizeCS, P, GroupCount);
			}
		}
	}

	// Kick a fresh readback of the committed positions for next frame's debug draw.
	AddEnqueueCopyPass(GraphBuilder, Resources->PositionReadback.Get(), Positions, sizeof(FVector3f) * Num);

	GraphBuilder.Execute();
}
