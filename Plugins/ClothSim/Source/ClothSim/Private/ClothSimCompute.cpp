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
	const TArray<float>& InitialInvMasses)
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

	// Colliders are constant within a frame; build the buffer once.
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

	// Distance-field collision uses the GDF snapshot captured by the view extension.
	const FClothGDFCache& GDFCache = ClothGDF::Get();
	const bool bDoDistanceField = Params.bUseDistanceFieldCollision && GDFCache.bValid;

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FClothPredictCS>       PredictCS(ShaderMap);
	TShaderMapRef<FClothSolveDistanceCS> SolveCS(ShaderMap);
	TShaderMapRef<FClothCollisionCS>     CollisionCS(ShaderMap);
	TShaderMapRef<FClothCollisionDFCS>   CollisionDFCS(ShaderMap);
	TShaderMapRef<FClothFinalizeCS>      FinalizeCS(ShaderMap);

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

			// --- Solve: ping-pong PredictedA <-> PredictedB ---
			FRDGBufferRef In = PredictedA;
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
