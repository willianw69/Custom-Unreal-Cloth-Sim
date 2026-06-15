# ARCHITECTURE.md

> Technical system documentation. Update whenever the architecture changes.
> Last updated: 2026-06-15 (after M9).

## High-Level Architecture

```
                       GAME THREAD                          RENDER THREAD (GPU)
  ┌─────────────────────────────────────────┐   ┌────────────────────────────────────┐
  │ UClothSimComponent (UMeshComponent)      │   │ ClothSimCompute (RDG passes)         │
  │  - sim params (grid, gravity, solver)    │   │                                      │
  │  - fixed-timestep accumulator            │   │  Persistent pooled buffers:          │
  │  - builds grid + topology + UVs          │──▶│   Positions, Velocities, InvMass,    │
  │  - builds + colors constraints (M7)      │   │   Constraints (color-sorted)         │
  │  - per fixed step: ENQUEUE dispatch ──────┼──▶│                                      │
  │  - reads readback → local verts+normals   │   │  Per frame / substep:                │
  │  - pushes verts to proxy ─────────────────┼──▶│   Predict → Solve(xN) → Finalize     │
  └─────────────────────────────────────────┘   │   → EnqueueCopy (readback)           │
                     │                            └────────────────┬─────────────────────┘
                     │ CreateSceneProxy                            │ positions
                     ▼                                             ▼
            ┌──────────────────────┐                   ┌────────────────────────┐
            │ FClothMeshSceneProxy │   lock+memcpy      │ FRHIGPUBufferReadback  │
            │ FLocalVertexFactory  │◀──────────────────│ (non-stalling)          │
            │ + index/UV/color VBs │   verts each frame └────────────────────────┘
            └──────────────────────┘
                     │ standard mesh draw
                     ▼  lit cloth on screen
```

## Simulation Pipeline (per fixed timestep, repeated `Substeps` times)
1. **Predict** (`ClothPredict.usf`): `v += gravity·dt`; **wind**: compute the particle's smooth
   normal from grid neighbours, then `v += WindDrag·dot(v_air − v, n)·n · dt` (v_air = wind +
   turbulence); then `v *= damping; predicted = x + v·dt`. Pinned particles keep their position.
2. **Solve** — one of two interchangeable strategies (`EClothSolverMode`, switchable at runtime):
   - **Jacobi** (`ClothSolveDistance.usf`, ×`SolverIterations`): one thread per particle gathers
     up to 8 grid neighbours (4 structural rest=`Spacing`, 4 shear rest=`Spacing·√2`), computes
     each PBD distance correction, averages (Jacobi under-relaxation), writes its own slot. Two
     predicted buffers are ping-ponged between iterations.
   - **Gauss-Seidel** (`ClothSolveGaussSeidel.usf`, ×`SolverIterations` × `NumColors`): solves the
     explicit, graph-colored constraint buffer **in place** on a single predicted buffer. One
     thread per constraint projects BOTH endpoints (mass-weighted); the dispatcher issues one pass
     per color. Within a color no constraint shares a particle (disjoint writes → race-free); RDG
     serializes color N+1 after N so later colors see earlier corrections — true Gauss-Seidel, no
     under-relaxation, faster convergence. Includes bending constraints (2-away, rest=`2·Spacing`).
2b. **Self-collision** (`ClothBuildGrid.usf` + `ClothSelfCollision.usf`, if enabled, ×
   `SelfCollisionIterations`): bin particles into a uniform spatial hash grid (atomic bucket append),
   then each particle scans its 27 neighbour cells and repels any **non-adjacent** particle closer
   than the thickness (Jacobi gather → ping-pong `PredictedA/B`, race-free). Runs before external
   colliders. Skips the 1-ring grid neighbours so it doesn't fight the distance constraints.
3. **Collision** (`ClothCollision.usf`, if any colliders OR the ground plane): in place on the solved predicted
   buffer, push each penetrating particle out to the collider surface (capsule = segment+radius;
   sphere = degenerate), then damp the tangential part of its motion for friction. Also projects
   particles above an optional infinite **ground plane** (normal +Z at `GroundZ`) with friction.
3b. **Distance-field collision** (`ClothCollisionDF.usf`, if enabled + GDF available): sample the
   Global Distance Field at each particle and push out along the gradient — collides with any
   scene mesh. Binds the standalone GDF params + a snapshotted View uniform buffer.
4. **Finalize** (`ClothFinalize.usf`): `v = (predicted − x)/dt; x = predicted`. Velocity is
   *derived* from the solved (and collided) motion — the source of PBD's stability.

### Constraint coloring (M7, Gauss-Seidel path)
Built once on the game thread in `UClothSimComponent::BuildConstraints`:
1. Emit edges — structural (right/down), shear (both cell diagonals), and optional bending (2-away
   right/down). Each is an `FGPUConstraint{IndexA, IndexB, RestLength, StiffScale}` (16 B).
2. **Greedy graph coloring:** for each edge assign the smallest color not already used by either
   endpoint (a `TSet<int32>` of used colors per particle). Constraints sharing a particle therefore
   land in different colors.
3. Bucket the edges into a single buffer **sorted by color**, recording a `[Start, Count)`
   `FClothColorRange` per color. Buffer + ranges are uploaded/stored on `FClothRenderResources` at
   init; the dispatcher walks the ranges every substep. `StiffScale` is 1 for structural/shear and
   `BendStiffness` for bending, multiplied by the global `Stiffness` uniform (clamped) in the shader.

### Self-collision spatial hash (M9)
Each substep (×`SelfCollisionIterations`), built fresh from the latest predicted positions:
1. **Build** (`ClothBuildGrid.usf`): cell = `floor(P / thickness)`, hashed (Teschner
   `73856093/19349663/83492791`) into a `TableSize`-bucket table (`TableSize` = next prime ≥ 2N).
   Each particle claims a slot with `InterlockedAdd(CellCounts[h])` and writes its index into
   `CellParticles[h*MaxPerCell + slot]` (overflow past `MaxPerCell`=16 dropped). `CellCounts` is a
   typed `Buffer<uint>` (clean `AddClearUAVPass` + atomics); `CellParticles` is a structured index list.
2. **Respond** (`ClothSelfCollision.usf`): each particle scans its 27 neighbour cells; for every
   non-`self`, non-1-ring-neighbour particle within `thickness`, accumulate a mass-weighted
   repulsion. Reads the input snapshot, writes its own slot in the other ping-pong buffer → race-free.
Thickness = `SelfCollisionScale·Spacing` (kept < 2·Spacing, the nearest non-adjacent rest length, so
flat cloth never self-fights). The only atomics in the project live in the build pass.

### Global Distance Field plumbing (M6)
The GDF is renderer-owned and only valid during scene rendering, so a minimal
`FClothSceneViewExtension` captures it each frame in `PostRenderBasePassDeferred_RenderThread`:
`UE::FXRenderingUtils::GetGlobalDistanceFieldParameterData(view)` → cached `FGlobalDistanceFieldParameterData`,
plus `FSceneView::ViewUniformBuffer` and `PreViewTranslation`. The (separately-enqueued) sim reads
this cache (1-frame lag is fine — GDF atlas is persistent) and binds it to the DF collision pass.
The DF shader uses the header's non-material branch, so GDF inputs come from
`FGlobalDistanceFieldParameters2`; `View` is bound only to satisfy transitive `ResolvedView` refs.

## Rendering Pipeline
- Topology (index buffer) + UVs are **static**, built once on the game thread.
- Each frame the component reads the latest world positions from the GPU readback, converts
  to local space, computes **smooth area-weighted normals + tangents** (CPU), and enqueues a
  render-thread update.
- `FClothMeshSceneProxy::UpdateVertices_RenderThread` writes positions + tangent basis (and, when
  strain visualization is on, per-vertex colors) into the `FStaticMeshVertexBuffers` and uploads via
  `LockBuffer`/`memcpy`.
- **Strain viz (M8):** the component computes per-particle membrane strain on the CPU from the
  position readback (signed deviation of structural-neighbour edge lengths from rest), maps it to a
  blue→green→red ramp, and passes the colors to the proxy (which uploads the `ColorVertexBuffer`).
  The same colors tint the debug points. Profiling visibility is via per-pass `RDG_EVENT_NAME`
  labels — NOT RHI breadcrumb scopes, which crash on this standalone RDG builder.
- A standard `FLocalVertexFactory` mesh batch is submitted in `GetDynamicMeshElements`, so
  lighting/shadows/material support come for free.

## RDG Pass Flow (one dispatch call = one fixed step)
```
FRDGBuilder
  RegisterExternalBuffer(Positions/Velocities/InvMass)   // persistent pooled buffers
  CreateBuffer(PredictedA), CreateBuffer(PredictedB)      // transient ping-pong
  if GaussSeidel: RegisterExternalBuffer(Constraints)    // persistent, color-sorted
  for s in [0..Substeps):
      AddPass  ClothPredict           (Positions,Velocities,InvMass) -> PredictedA
      // --- Solve (one of two paths); `In` holds the result either way ---
      if Jacobi:
          In=A, Out=B
          for it in [0..SolverIterations):
              AddPass ClothSolveDistance     (PredictedIn,InvMass) -> PredictedOut ; swap(In,Out)
      else GaussSeidel:                                  // in place on PredictedA (=In)
          for it in [0..SolverIterations):
              for c in [0..NumColors):
                  AddPass ClothSolveGaussSeidel (Constraints[c range], InvMass, In[UAV])  // race-free
      if NumColliders: AddPass ClothCollision (In[UAV], PrevPos=Positions, Colliders)  // in place
      AddPass  ClothFinalize          (PredictedIn=In, InvMass) -> Positions, Velocities
  AddEnqueueCopyPass(PositionReadback, Positions)         // for debug + mesh render
GraphBuilder.Execute()
```

## GPU Buffers
| Buffer | Type | Lifetime | Stride | Notes |
|---|---|---|---|---|
| Positions | StructuredBuffer<float3> | persistent (pooled) | 12 B | world space |
| Velocities | StructuredBuffer<float3> | persistent (pooled) | 12 B | cm/s |
| InvMasses | StructuredBuffer<float> | persistent (pooled) | 4 B | 0 = pinned |
| PredictedA/B | StructuredBuffer<float3> | transient (per frame) | 12 B | Jacobi ping-pong; GS uses only A (in place) |
| Constraints | StructuredBuffer<FGPUConstraint> | persistent (pooled) | 16 B | idxA,idxB,rest,stiffScale; color-sorted (GS) |
| Colliders | StructuredBuffer<FCollider> | transient (per frame) | 32 B | A,radius,B,friction |
| CellCounts | Buffer<uint> (typed) | transient (per self-collide iter) | 4 B | hash bucket particle counts; ClearUAV + atomics |
| CellParticles | StructuredBuffer<uint> | transient (per self-collide iter) | 4 B | TableSize·MaxPerCell index lists |
| PositionReadback | FRHIGPUBufferReadback | persistent | — | non-stalling CPU copy |

Render-side vertex buffers (position, tangents, UV, color, index) live in the scene proxy
via `FStaticMeshVertexBuffers` and are updated by CPU lock+memcpy.

## CPU / GPU Responsibilities
- **CPU (game thread):** parameters, grid + topology construction, fixed-step accumulation,
  readback → local-space conversion, normal/tangent computation, proxy vertex update enqueue.
- **GPU (render thread):** all simulation math (predict/solve/finalize), the readback copy.

## Shader Responsibilities
- `ClothPredict.usf` — external forces (gravity + normal-dependent wind/drag + turbulence) and
  position prediction. Computes per-particle normals on the fly from grid neighbours.
- `ClothSolveDistance.usf` — distance-constraint relaxation (Jacobi per-particle gather).
- `ClothSolveGaussSeidel.usf` — one constraint per thread over a color range; projects both
  endpoints in place (graph-colored Gauss-Seidel, structural+shear+bending).
- `ClothCollision.usf` — project predicted positions out of sphere/capsule colliders + the optional ground plane + friction.
- `ClothBuildGrid.usf` — bin particles into the self-collision spatial hash grid (atomic append).
- `ClothSelfCollision.usf` — repel close non-adjacent particles using the grid (Jacobi gather).
- `ClothFinalize.usf` — commit positions, derive velocity.
- (Shader virtual path root `/ClothSim` → `Plugins/ClothSim/Shaders`, mapped at module
  startup in `FClothSimModule::StartupModule`; files referenced as `/ClothSim/Private/...`.)

## Data Layouts
- Particle index `i = y * GridWidth + x`. Grid laid out per `Orientation`: **vertical curtain** in
  the X-Z plane (width +X, height −Z, row 0 = top) or **horizontal sheet** in the X-Y plane (drops
  flat). Pinned: top corners by default. The horizontal sheet uses reversed triangle winding so its
  geometric front faces up.
- **Normal handedness:** smooth normals are `Cross(E2,E1)` (not `Cross(E1,E2)`) so they agree with
  Unreal's left-handed front-face winding. Two-sided materials flip the vertex normal by winding
  (VFACE); a mismatched (right-handed) normal makes the visible face shade black under all lights.
- `float3` structured buffers use a tight 12-byte stride (matches `FVector3f`). The 16-byte
  alignment trap applies to **constant** buffers, not structured buffers.

## Dependency Relationships
- Solve passes depend on Predict (RDG tracks read/write of predicted buffers).
- Finalize depends on the last Solve iteration.
- Mesh render depends on the readback, which depends on Finalize.
- The scene proxy holds no sim state; it only receives finished vertex data.
