# PROJECT_STATE.md

> Single source of truth for current project status. Update after every milestone.
> Last updated: 2026-06-15 (after M8).

## Project Overview
Real-time **GPU cloth simulation built from scratch** in **Unreal Engine 5.7**, as a
Technical Artist portfolio piece. The cloth is simulated entirely with **custom compute
shaders** (no Chaos Cloth). Position/velocity live in GPU structured buffers, constraints
are solved on the GPU (XPBD/PBD), and the result is rendered as a dynamic lit mesh.

- **Host project:** `ClothSimDemo` (UE 5.7 C++).
- **Plugin (all the real work):** `Plugins/ClothSim`.
- **Engine install:** `E:\Epic Games\UE_5.7`.

## Current Milestone
**M8 — Debug Viz Polish + Profiling: COMPLETE and verified in-editor.**
Strain visualization (vertex colors + debug points), on-screen solver/constraint stats, and
per-pass profiling labels. Core roadmap M1–M8 is now complete.

## Completed Milestones
- **M1 — Particle Simulation.** GPU integration (gravity + damping), structured buffers,
  RDG dispatch, debug-point visualization via non-stalling readback.
- **M2 — XPBD Distance Constraints.** Predict → Solve (Jacobi per-particle gather of
  structural + shear constraints) → Finalize, with substepping. Cloth hangs from pinned
  corners. Includes the fixed-timestep fix (frame-rate independence).
- **M3 — Cloth Rendering.** `UClothSimComponent` is now a `UMeshComponent` with a custom
  `FClothMeshSceneProxy` (FLocalVertexFactory + FStaticMeshVertexBuffers). Lit fabric
  surface with CPU-computed smooth normals; vertices sourced from the GPU sim via a small
  readback.
- **M4 — Wind Forces.** Normal-dependent aerodynamic drag in the Predict pass
  (`F = WindDrag · dot(v_air−v_cloth, n) · n`), with per-particle GPU normals and animated
  turbulence. Cloth billows/ripples. Exposed Wind direction/strength/drag/turbulence.
- **M5 — Sphere & Capsule Collision.** GPU collider buffer (unified capsule = segment+radius;
  sphere = degenerate). `ClothCollision.usf` pass after the solver projects predicted positions
  out of colliders and damps tangential motion (friction). Editable collider slots + `bPinTopEdge`.
- **M6 — Distance Field Mesh Collision.** Cloth collides with ANY scene mesh via Unreal's Global
  Distance Field. A SceneViewExtension snapshots the GDF params + view uniform buffer each frame;
  `ClothCollisionDF.usf` samples `GetDistanceToNearestSurfaceGlobal` + gradient to push particles
  out of arbitrary geometry. Toggle `bUseDistanceFieldCollision`.
- **M7 — Colored Gauss-Seidel + Bending.** Explicit `FGPUConstraint` buffer (structural + shear +
  bending), greedy graph-colored on the CPU so each color is a race-free batch.
  `ClothSolveGaussSeidel.usf` projects one constraint per thread, one dispatch per color (RDG
  serializes colors → true Gauss-Seidel ordering, faster convergence than the Jacobi gather).
  Bending = 2-away distance constraint with relative `BendStiffness`. `EClothSolverMode` toggles
  Jacobi ↔ Gauss-Seidel at runtime; Jacobi is kept as the profiling baseline.
- **M8 — Debug Viz Polish + Profiling.** Strain visualization: per-particle membrane stretch →
  color ramp (blue=compressed, green=rest, red=stretched), uploaded to the mesh vertex-color buffer
  each frame AND to the debug points (`bVisualizeStrain`/`StrainScale`). On-screen stats readout
  (`bShowStats`): solver mode, particle/constraint/color counts, solve dispatches/substep. Per-pass
  `RDG_EVENT_NAME` labels give `ProfileGPU`/RenderDoc/Insights timing per pass.

## In-Progress Work
- None. Core roadmap (M1–M8) complete; remaining items are stretch goals (see below).

## Next Milestone
**M+ (stretch) — Zero-copy GPU rendering path**, and/or further solver work (XPBD compliance,
true dihedral bending). The functional roadmap is complete; these are polish/depth upgrades.

## Technical Decisions
- **Wind model:** normal-dependent aerodynamic drag computed in the Predict pass; per-particle
  normals derived on the GPU from grid neighbours (no extra pass); sinusoidal turbulence.
- **Collision model:** unified capsule (segment+radius); positional projection + tangential
  friction; runs as its own pass after the solver, in place on the predicted buffer (no races).
- **Distance-field collision:** GDF accessed via `UE::FXRenderingUtils::GetGlobalDistanceFieldParameterData`
  from a SceneViewExtension (captured in `PostRenderBasePassDeferred`, when the GDF exists). GDF
  inputs bound via the standalone `FGlobalDistanceFieldParameters2`; the engine header transitively
  needs the `View` UB (for `ResolvedView`), so we also snapshot `FSceneView::ViewUniformBuffer` and
  bind it. Sampling uses translated world space (world + cached PreViewTranslation). A 1-frame lag
  between the SVE snapshot and the (separately-enqueued) sim is harmless.
- **Solver model:** XPBD/PBD. Velocity is derived from the position delta (stable). Two
  interchangeable solvers (M7): **Jacobi gather** (per-particle, structural+shear) and
  **graph-colored Gauss-Seidel** (explicit constraint buffer, structural+shear+bending).
- **Parallelism:** Jacobi via **per-particle gather** of grid neighbours (no atomics, no races).
  Gauss-Seidel via **CPU graph coloring** → one thread per constraint, one dispatch per color:
  writes are disjoint within a color (race-free) and RDG serializes color N+1 after N, preserving
  Gauss-Seidel ordering. No under-relaxation needed → faster convergence.
- **Bending:** distance constraint to the 2-away neighbour (rest = 2·Spacing), with a relative
  `BendStiffness` baked per constraint and scaled by the global `Stiffness` uniform. GS path only.
- **Debug/profiling:** strain colors computed on the CPU from the position readback (reusing the
  data we already copy back) and pushed to the mesh's vertex-color buffer + debug points.
  Profiling visibility comes from per-pass `RDG_EVENT_NAME` labels (`ProfileGPU`/RenderDoc/Insights),
  NOT from `RDG_GPU_STAT_SCOPE`/`RDG_EVENT_SCOPE` — those open RHI breadcrumbs that are unbalanced
  on this plugin's standalone `FRDGBuilder` (driven from a render command on the immediate list) and
  crash at `Execute()`. See DEVLOG 2026-06-15 M8.
- **Timestep:** fixed-step accumulator (default 1/60 s) → frame-rate independent.
- **Rendering:** reliable path = `FLocalVertexFactory` updated from a small CPU readback
  (negligible at current vertex counts). Zero-copy GPU vertex write is a planned upgrade.
- **Spaces:** sim runs in **world space**; positions converted to local for the mesh proxy.

## Known Issues
- Default material is **one-sided** → cloth invisible from the back. Use a Two-Sided material.
- Rendering lags the true GPU state by ~1–2 frames (readback latency). Cosmetic only.
- DF collision requires **Generate Mesh Distance Fields** (Project Settings) + a GDF consumer.
  We force it via `r.DistanceFieldAO=1`/`r.AOGlobalDistanceField=1` in DefaultEngine.ini, which
  may slightly affect scene lighting. The Global Distance Field is **coarse**, so DF collision is
  soft/approximate on thin or small objects (great for large meshes).

## Known Limitations
- Collision is against authored sphere/capsule slots only (no floor/world geometry, no
  self-collision). Floor = add a large sphere/capsule for now.
- Bending constraints exist in the Gauss-Seidel path only; the Jacobi gather is structural+shear.
- The **Jacobi** solver assumes a **regular grid** (neighbours from grid coords); the Gauss-Seidel
  path uses an explicit constraint buffer and is not grid-bound, but the constraints are still
  *generated* from the grid topology.
- Constraint topology (incl. bending on/off) is baked at `BeginPlay`; changing `bUseBending`/
  `BendStiffness` requires a replay (grid params already behave this way).
- Normals computed on CPU (M3 reliable path), not GPU.
- Rendering is readback-based, not zero-copy.

## Future Improvements
- Zero-copy GPU vertex write (compute → UAV vertex buffers, custom vertex factory).
- True **dihedral** bending (angle-based) instead of the current 2-away distance approximation;
  XPBD compliance per constraint instead of a clamped PBD stiffness scale.
- GPU normals pass.
- Sphere/capsule (M5) and self-collision.
- Profiling pass with Unreal Insights / `stat GPU` capture for the portfolio.
