# ARCHITECTURE.md

> Technical system documentation. Update whenever the architecture changes.
> Last updated: 2026-06-12 (after M4).

## High-Level Architecture

```
                       GAME THREAD                          RENDER THREAD (GPU)
  ┌─────────────────────────────────────────┐   ┌────────────────────────────────────┐
  │ UClothSimComponent (UMeshComponent)      │   │ ClothSimCompute (RDG passes)         │
  │  - sim params (grid, gravity, solver)    │   │                                      │
  │  - fixed-timestep accumulator            │   │  Persistent pooled buffers:          │
  │  - builds grid + topology + UVs          │──▶│   Positions, Velocities, InvMass     │
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
2. **Solve** (`ClothSolveDistance.usf`, ×`SolverIterations`): one thread per particle gathers
   up to 8 grid neighbours (4 structural rest=`Spacing`, 4 shear rest=`Spacing·√2`), computes
   each PBD distance correction, averages (Jacobi under-relaxation), writes its own slot.
   Two predicted buffers are ping-ponged between iterations.
3. **Finalize** (`ClothFinalize.usf`): `v = (predicted − x)/dt; x = predicted`. Velocity is
   *derived* from the solved motion — the source of PBD's stability.

## Rendering Pipeline
- Topology (index buffer) + UVs are **static**, built once on the game thread.
- Each frame the component reads the latest world positions from the GPU readback, converts
  to local space, computes **smooth area-weighted normals + tangents** (CPU), and enqueues a
  render-thread update.
- `FClothMeshSceneProxy::UpdateVertices_RenderThread` writes positions + tangent basis into
  the `FStaticMeshVertexBuffers` and uploads via `LockBuffer`/`memcpy`.
- A standard `FLocalVertexFactory` mesh batch is submitted in `GetDynamicMeshElements`, so
  lighting/shadows/material support come for free.

## RDG Pass Flow (one dispatch call = one fixed step)
```
FRDGBuilder
  RegisterExternalBuffer(Positions/Velocities/InvMass)   // persistent pooled buffers
  CreateBuffer(PredictedA), CreateBuffer(PredictedB)      // transient ping-pong
  for s in [0..Substeps):
      AddPass  ClothPredict           (Positions,Velocities,InvMass) -> PredictedA
      In=A, Out=B
      for it in [0..SolverIterations):
          AddPass ClothSolveDistance  (PredictedIn,InvMass) -> PredictedOut ; swap(In,Out)
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
| PredictedA/B | StructuredBuffer<float3> | transient (per frame) | 12 B | solver ping-pong |
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
- `ClothSolveDistance.usf` — distance-constraint relaxation (Jacobi gather).
- `ClothFinalize.usf` — commit positions, derive velocity.
- (Shader virtual path root `/ClothSim` → `Plugins/ClothSim/Shaders`, mapped at module
  startup in `FClothSimModule::StartupModule`; files referenced as `/ClothSim/Private/...`.)

## Data Layouts
- Particle index `i = y * GridWidth + x`. Grid laid out in component-local X-Z plane:
  width along +X, height down −Z (row 0 = top). Pinned: top corners by default.
- `float3` structured buffers use a tight 12-byte stride (matches `FVector3f`). The 16-byte
  alignment trap applies to **constant** buffers, not structured buffers.

## Dependency Relationships
- Solve passes depend on Predict (RDG tracks read/write of predicted buffers).
- Finalize depends on the last Solve iteration.
- Mesh render depends on the readback, which depends on Finalize.
- The scene proxy holds no sim state; it only receives finished vertex data.
