// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h" // TRefCountPtr<FRDGPooledBuffer>

class FRHIGPUBufferReadback;
class FRHICommandListImmediate;

/**
 * Parameters pushed from the game thread to the render thread each frame.
 * Plain value type: it is copied into the render command lambda, so it must not
 * hold raw pointers to game-thread-owned UObjects.
 */
/**
 * One collider, as the GPU sees it. Both shapes are represented as a CAPSULE
 * (a line segment A-B with a radius); a SPHERE is just the degenerate case A == B.
 * This lets a single collision routine handle both. Must match the HLSL `FCollider`
 * struct layout in ClothCollision.usf (32 bytes).
 */
struct FGPUCollider
{
	FVector3f A = FVector3f::ZeroVector;
	float     Radius = 0.0f;
	FVector3f B = FVector3f::ZeroVector;
	float     Friction = 0.0f; // [0..1] tangential velocity damping on contact
};

/**
 * One explicit distance constraint, as the GPU Gauss-Seidel solver sees it (M7).
 * A and B are particle indices; the solver projects them back to RestLength apart.
 * StiffScale is a per-constraint relative stiffness (1 for structural/shear, lower
 * for bending) that is multiplied by the global Stiffness uniform. Must match the
 * HLSL `FConstraint` struct in ClothSolveGaussSeidel.usf (16 bytes, tight layout).
 */
struct FGPUConstraint
{
	uint32 IndexA = 0;
	uint32 IndexB = 0;
	float  RestLength = 0.0f;
	float  StiffScale = 1.0f;
};

/** Contiguous [Start, Count) span of one color within the sorted constraint buffer. */
struct FClothColorRange
{
	int32 Start = 0;
	int32 Count = 0;
};

struct FClothSimParams
{
	int32   NumParticles = 0;
	int32   GridWidth = 0;
	int32   GridHeight = 0;

	float   DeltaTime = 1.0f / 60.0f;
	FVector3f Gravity = FVector3f(0.0f, 0.0f, -980.0f);
	float   Damping = 0.1f;

	// XPBD/PBD solver controls
	int32   Substeps = 2;          // split DeltaTime for stability (biggest quality lever)
	int32   SolverIterations = 8;  // distance-constraint relaxation passes per substep
	float   Stiffness = 1.0f;      // [0,1] correction scale
	float   RestStructural = 5.0f; // adjacent-particle rest length (= Spacing)
	float   RestShear = 7.0710678f;// diagonal rest length (= Spacing * sqrt(2))

	// Solver method (M7). false = Jacobi per-particle gather (baseline, structural+shear
	// from grid neighbours). true = graph-colored Gauss-Seidel over the explicit
	// constraint buffer (structural+shear+bending; faster convergence).
	bool    bUseGaussSeidel = false;

	// Wind (M4)
	FVector3f WindVelocity = FVector3f::ZeroVector; // base air velocity (cm/s), world space
	float   WindDrag = 1.0f;       // aerodynamic coefficient (how strongly air pushes the face)
	float   WindTurbulence = 0.0f; // [0..1] fraction of WindVelocity added as animated gusting
	float   TimeSeconds = 0.0f;    // animation clock for turbulence noise

	// Collision (M5) — world-space colliders rebuilt each frame.
	TArray<FGPUCollider> Colliders;
	float   Friction = 0.3f;

	// Distance-field collision (M6) — collide against any scene mesh via the GDF.
	bool    bUseDistanceFieldCollision = false;
	float   DFThickness = 2.0f; // contact shell thickness (cm)

	// Self-collision (M9) — cloth vs itself via a spatial hash grid.
	bool    bSelfCollision = false;
	float   SelfThickness = 5.0f;  // min separation between non-adjacent particles (cm)
	float   SelfStiffness = 1.0f;  // [0,1] repulsion strength
	int32   SelfCollisionIterations = 2; // repulsion passes per substep (deeper stacks)

	// Built-in ground plane (M9) — infinite floor at world Z = GroundZ (normal +Z).
	bool    bGroundPlane = false;
	float   GroundZ = 0.0f;
};

/**
 * Render-thread-owned GPU state for one cloth instance.
 *
 * The simulation is stateful: this frame's positions/velocities are next frame's
 * input. RDG resources are transient (recreated every FRDGBuilder), so we cannot
 * keep simulation state "inside" the graph. Instead we keep persistent pooled
 * buffers here and RegisterExternalBuffer() them into the graph each frame.
 *
 * Lifetime: created on the game thread, but every member is only ever touched on
 * the render thread. Held by TSharedPtr so render command lambdas can safely keep
 * it alive even if the owning component is destroyed mid-flight.
 */
struct FClothRenderResources
{
	TRefCountPtr<FRDGPooledBuffer> PositionsBuffer;
	TRefCountPtr<FRDGPooledBuffer> VelocitiesBuffer;
	TRefCountPtr<FRDGPooledBuffer> InvMassBuffer;

	// Explicit distance constraints + their graph coloring (M7, Gauss-Seidel path).
	// Built once at init; static for the cloth's lifetime. The buffer is sorted by
	// color so each ColorRange is a contiguous, race-free batch of constraints.
	TRefCountPtr<FRDGPooledBuffer> ConstraintsBuffer;
	int32 NumConstraints = 0;
	TArray<FClothColorRange> ColorRanges;

	int32 NumParticles = 0;
	bool  bInitialized = false;

	// --- Debug readback (TEMPORARY, M1 only) -------------------------------
	// A real GPU-resident renderer (M3) will read the position buffer directly in
	// a vertex factory. For bring-up we copy positions back to the CPU with a
	// fenced, non-stalling readback and draw them with DrawDebugPoint so we can
	// SEE that the compute pipeline works. This readback is on the critical path
	// for validation only and will be removed.
	TUniquePtr<FRHIGPUBufferReadback> PositionReadback;

	// Latest CPU-side copy of positions, guarded for game-thread debug drawing.
	FCriticalSection     DebugCopyCS;
	TArray<FVector3f>    DebugPositions;
	bool                 bHasDebugData = false;

	// Declared out-of-line (defined in ClothSimCompute.cpp). TUniquePtr<FRHIGPUBufferReadback>
	// needs the COMPLETE type to generate its destructor; only that .cpp includes it.
	// Without this, MakeShared<> in the component would try to instantiate the dtor
	// against a forward declaration -> C4150.
	FClothRenderResources();
	~FClothRenderResources();
};

/**
 * Compute-side entry points. All run on the render thread.
 */
namespace ClothSimCompute
{
	/** One-time: create pooled buffers and upload the initial particle grid + constraints.
	 *  Constraints must already be sorted by color; ColorRanges indexes into them. */
	void InitResources_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const TSharedPtr<FClothRenderResources>& Resources,
		const TArray<FVector3f>& InitialPositions,
		const TArray<FVector3f>& InitialVelocities,
		const TArray<float>& InitialInvMasses,
		const TArray<FGPUConstraint>& Constraints,
		const TArray<FClothColorRange>& ColorRanges);

	/** Per-frame: run the integration compute pass and kick a debug readback. */
	void Dispatch_RenderThread(
		FRHICommandListImmediate& RHICmdList,
		const TSharedPtr<FClothRenderResources>& Resources,
		const FClothSimParams& Params);
}
