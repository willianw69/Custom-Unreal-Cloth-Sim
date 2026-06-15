// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "ClothSimComponent.generated.h"

struct FClothRenderResources;
struct FGPUConstraint;
struct FClothColorRange;
class FClothMeshSceneProxy;
class UMaterialInterface;

UENUM(BlueprintType)
enum class EClothColliderType : uint8
{
	Sphere,
	Capsule
};

/** Which GPU distance solver to run (M7). */
UENUM(BlueprintType)
enum class EClothSolverMode : uint8
{
	/** Per-particle gather of grid neighbours, Jacobi-averaged. Baseline (M2). */
	Jacobi			UMETA(DisplayName = "Jacobi (gather)"),
	/** Graph-colored Gauss-Seidel over the explicit constraint buffer. Faster convergence (M7). */
	GaussSeidel		UMETA(DisplayName = "Gauss-Seidel (colored)")
};

/** A single collider authored in the Details panel (transform relative to the component). */
USTRUCT(BlueprintType)
struct FClothCollider
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collider")
	EClothColliderType Type = EClothColliderType::Sphere;

	/** Center offset from the component origin (local space). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collider")
	FVector Center = FVector(0.0f, 0.0f, -100.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collider", meta = (ClampMin = "0.1"))
	float Radius = 30.0f;

	/** Capsule only: half the distance between the two end caps, along the local axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collider", meta = (ClampMin = "0.0"))
	float HalfHeight = 50.0f;

	/** Capsule only: orientation of the capsule axis (local up = capsule length). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collider")
	FRotator Rotation = FRotator::ZeroRotator;
};

/**
 * UClothSimComponent
 *
 * Owns a grid of particles simulated on the GPU (XPBD distance constraints) and
 * renders the result as a real lit mesh. It is a UMeshComponent so it gets its own
 * scene proxy, material support, and participates in lighting/shadows.
 *
 * Threading model (important):
 *   - Game thread: builds the initial grid + topology, owns the editable parameters,
 *     each tick pushes a parameter snapshot to the render thread, and feeds the latest
 *     positions (from a small GPU readback) into the mesh scene proxy.
 *   - Render thread: owns all GPU sim resources and runs the compute passes.
 * They share state only through ENQUEUE_RENDER_COMMAND and a TSharedPtr.
 *
 * NOTE: rendering currently sources vertex positions from a small CPU readback for
 * reliability. A zero-copy path (compute writes the vertex buffers directly) is a
 * planned upgrade; the simulation itself is already fully GPU-resident.
 */
UCLASS(ClassGroup = (ClothSim), meta = (BlueprintSpawnableComponent))
class CLOTHSIM_API UClothSimComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UClothSimComponent();

	/** Particles across the cloth width. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ClothSim|Grid", meta = (ClampMin = "2", ClampMax = "256"))
	int32 GridWidth = 32;

	/** Particles down the cloth height. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ClothSim|Grid", meta = (ClampMin = "2", ClampMax = "256"))
	int32 GridHeight = 32;

	/** Distance between adjacent particles (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ClothSim|Grid", meta = (ClampMin = "0.1"))
	float Spacing = 5.0f;

	/** Pin the two top corners so the cloth hangs (M1 sanity check). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ClothSim|Grid")
	bool bPinTopCorners = true;

	/** Pin the entire top row instead of just the corners (good for draping over colliders). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ClothSim|Grid")
	bool bPinTopEdge = false;

	/** Acceleration applied to free particles (cm/s^2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Physics")
	FVector Gravity = FVector(0.0f, 0.0f, -980.0f);

	/** Per-second linear velocity damping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Physics", meta = (ClampMin = "0.0"))
	float Damping = 0.1f;

	/** Wind direction (world space; auto-normalized). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Wind")
	FVector WindDirection = FVector(1.0f, 0.0f, 0.0f);

	/** Wind speed (cm/s). 0 disables wind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Wind", meta = (ClampMin = "0.0"))
	float WindStrength = 0.0f;

	/** Aerodynamic drag coefficient: how strongly air pushes the cloth faces it hits. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Wind", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float WindDrag = 1.0f;

	/** Gustiness [0..1]: fraction of wind added as animated turbulence. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Wind", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WindTurbulence = 0.5f;

	/** Sphere/capsule colliders the cloth collides against (transforms relative to this component). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Collision")
	TArray<FClothCollider> Colliders;

	/** Contact friction [0..1]: how strongly the cloth grips a collider surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Collision", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Friction = 0.3f;

	/** Draw wireframe shapes for the colliders (they are otherwise invisible math). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Collision")
	bool bDrawColliders = true;

	/**
	 * Collide against ANY mesh in the scene using Unreal's Global Distance Field
	 * (in addition to the explicit collider slots). Requires "Generate Mesh Distance
	 * Fields" enabled in Project Settings and meshes that have distance fields.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Collision")
	bool bUseDistanceFieldCollision = false;

	/** Contact shell thickness for distance-field collision (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Collision", meta = (ClampMin = "0.0"))
	float DistanceFieldThickness = 2.0f;

	/**
	 * Distance solver method. Jacobi = per-particle neighbour gather (baseline).
	 * Gauss-Seidel = graph-colored explicit constraints (faster convergence, and the
	 * only path that includes bending). Switchable at runtime for side-by-side comparison.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Solver")
	EClothSolverMode SolverMode = EClothSolverMode::GaussSeidel;

	/**
	 * Add bending constraints (distance to the 2-away neighbour) so the cloth resists
	 * sharp folds. Only active in the Gauss-Seidel path. Built at BeginPlay.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ClothSim|Solver")
	bool bUseBending = true;

	/** Relative stiffness of bending constraints [0..1] vs structural/shear. Built at BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ClothSim|Solver", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BendStiffness = 0.2f;

	/** Substeps per frame. The biggest stability lever: more = stiffer, more stable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Solver", meta = (ClampMin = "1", ClampMax = "16"))
	int32 Substeps = 2;

	/** Constraint relaxation iterations per substep. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Solver", meta = (ClampMin = "1", ClampMax = "64"))
	int32 SolverIterations = 8;

	/** Correction strength [0..1]. 1 = try to fully satisfy constraints each iteration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Solver", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Stiffness = 1.0f;

	/**
	 * Fixed simulation timestep (seconds). The sim always advances in chunks of this
	 * size regardless of the editor/game frame rate, which makes the result
	 * frame-rate independent and stable. 1/60 is a good default.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Solver", meta = (ClampMin = "0.002", ClampMax = "0.05"))
	float FixedTimeStep = 1.0f / 60.0f;

	/** Safety cap on fixed steps per frame so a hitch can't trigger a "spiral of death". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Solver", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxStepsPerFrame = 4;

	/** Material applied to the cloth. If null, the engine default surface is used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Render")
	TObjectPtr<UMaterialInterface> ClothMaterial = nullptr;

	/** Also draw each particle as a debug point on top of the mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Debug")
	bool bDrawDebugPoints = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Debug", meta = (ClampMin = "0.5"))
	float DebugPointSize = 4.0f;

	/**
	 * Color the cloth by membrane strain (stretch): blue = compressed, green = at rest,
	 * red = stretched. Written to the mesh's vertex colors AND to the debug points.
	 * To see it on the lit surface, the assigned material must route Vertex Color into
	 * Base Color (or Emissive); the debug points show strain regardless of material.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Debug")
	bool bVisualizeStrain = false;

	/** Strain magnitude that maps to full blue/red (e.g. 0.2 = 20% stretch is fully red). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Debug", meta = (ClampMin = "0.01"))
	float StrainScale = 0.25f;

	/** Show an on-screen readout of solver mode + particle/constraint/color/dispatch counts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ClothSim|Debug")
	bool bShowStats = false;

	//~ UPrimitiveComponent / UMeshComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override { return 1; }
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** Build the particle grid (world + local), topology and UVs, then upload to the GPU. */
	void InitializeSimulation();

	/** Build the static triangle list + UVs for the grid. */
	void BuildTopology();

	/** Build the explicit distance constraints (structural + shear + optional bending) and
	 *  greedily graph-color them, producing a color-sorted buffer + per-color ranges (M7). */
	void BuildConstraints(TArray<FGPUConstraint>& OutConstraints, TArray<FClothColorRange>& OutColorRanges) const;

	/** Compute smooth per-vertex normals and tangents from grid positions (local space). */
	void ComputeGridNormalsTangents(
		const TArray<FVector3f>& InPositions,
		TArray<FVector3f>& OutNormals,
		TArray<FVector3f>& OutTangents) const;

	/** Compute a per-particle strain color (blue=compressed, green=rest, red=stretched)
	 *  from structural-neighbour edge lengths vs the rest Spacing. */
	void ComputeStrainColors(const TArray<FVector3f>& InPositions, TArray<FColor>& OutColors) const;

	/** Pull the latest readback positions, convert to local space, recompute normals,
	 *  and push the updated vertices to the scene proxy. */
	void UpdateMeshFromSimulation();

	/** Draw readback positions as debug points (optional). */
	void DrawDebug();

	/** Draw wireframe shapes for the authored colliders so they're visible. */
	void DrawColliders();

	int32 NumParticles = 0;

	/** Unsimulated real time carried over between frames for the fixed-step loop. */
	float TimeAccumulator = 0.0f;

	// Static topology (built once).
	TArray<uint32>    Triangles;
	TArray<FVector2f> UV0;

	// Initial local-space positions (corner at component origin); used to seed the proxy.
	TArray<FVector3f> InitialLocalPositions;

	// Scratch reused each frame for the proxy update (local space).
	TArray<FVector3f> LocalPositions;
	TArray<FVector3f> LocalNormals;
	TArray<FVector3f> LocalTangents;
	TArray<FColor>    LocalColors;   // per-vertex strain colors (M8)

	// Constraint stats captured at BeginPlay for the on-screen readout (M8).
	int32 NumConstraintsBuilt = 0;
	int32 NumColorsBuilt = 0;

	FBoxSphereBounds LocalBounds = FBoxSphereBounds(ForceInit);

	// Shared with render command lambdas so GPU work outlives this component safely.
	TSharedPtr<FClothRenderResources> RenderResources;
};
