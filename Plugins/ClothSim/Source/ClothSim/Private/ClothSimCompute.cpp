// Copyright Epic Games, Inc. All Rights Reserved.

#include "ClothSimResources.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "DataDrivenShaderPlatformInfo.h"

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
		SHADER_PARAMETER(float, SubDeltaTime)
		SHADER_PARAMETER(FVector3f, Gravity)
		SHADER_PARAMETER(float, Damping)
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

IMPLEMENT_GLOBAL_SHADER(FClothPredictCS,        "/ClothSim/Private/ClothPredict.usf",        "MainCS", SF_Compute);
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

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FClothPredictCS>       PredictCS(ShaderMap);
	TShaderMapRef<FClothSolveDistanceCS> SolveCS(ShaderMap);
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
				P->SubDeltaTime       = SubDt;
				P->Gravity            = Params.Gravity;
				P->Damping            = Params.Damping;

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
