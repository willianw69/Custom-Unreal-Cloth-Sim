# HANDOFF.md

> For a new session/engineer to continue immediately. Assume zero prior context.
> Update after every milestone — always represents the current state.
> Last updated: 2026-06-15 (after M9).

## Project Summary
From-scratch **GPU cloth simulation in UE 5.7** (no Chaos Cloth). Custom compute shaders do
XPBD/PBD on structured buffers; rendered as a dynamic lit mesh. Portfolio project for a TA.
Host project `ClothSimDemo`, all work in `Plugins/ClothSim`. Engine: `E:\Epic Games\UE_5.7`.

## Current Milestone
M9 (Self-Collision + drop-test support) complete and verified in-editor. **M1–M9 done.**
Remaining work is stretch goals (M+: zero-copy rendering; continuous self-collision CCD; XPBD compliance).

## Completed Work
- M1: GPU integration + structured buffers + RDG + debug points.
- M2: XPBD distance constraints (Jacobi per-particle gather) + substepping + fixed timestep.
- M3: `UMeshComponent` + `FClothMeshSceneProxy` (FLocalVertexFactory), lit cloth, CPU normals.
- M4: normal-dependent aerodynamic wind + turbulence in the Predict pass (GPU normals).
- M5: sphere/capsule collision (`ClothCollision.usf`) + friction; editable collider slots.
- M6: distance-field collision vs any scene mesh (`ClothCollisionDF.usf` + `FClothSceneViewExtension`).
- M7: graph-colored Gauss-Seidel solver (`ClothSolveGaussSeidel.usf`) over an explicit, CPU-colored
  constraint buffer + bending constraints; `EClothSolverMode` toggles vs the Jacobi baseline.
- M8: strain visualization (vertex colors + debug points), on-screen solver/constraint stats, and
  per-pass GPU profiling labels (`bVisualizeStrain`/`StrainScale`/`bShowStats`).
- M9: self-collision via spatial hash (`ClothBuildGrid.usf` + `ClothSelfCollision.usf`); cloth
  `Orientation` (vertical/horizontal); built-in ground plane; self-collision debug overlay; two-sided
  normal-handedness fix.

## Current Technical Decisions
- PBD/XPBD; velocity derived from position delta.
- Two solvers (M7), runtime-switchable via `EClothSolverMode`:
  - **Jacobi** per-particle gather of grid neighbours (no atomics) — baseline.
  - **Graph-colored Gauss-Seidel** over an explicit constraint buffer — one thread/constraint, one
    dispatch/color (disjoint writes within a color, RDG-serialized across colors) → faster
    convergence; the only path with bending constraints.
- Fixed-timestep accumulator (1/60 s) for frame-rate independence.
- Sim in world space; converted to local for the mesh.
- Rendering via readback → FLocalVertexFactory (reliable). Zero-copy is a future upgrade.

## Known Issues
- Use a **Two-Sided** material for cloth (it's a thin two-sided surface). Smooth normals are built
  `Cross(E2,E1)` to match UE's left-handed winding so two-sided shading lights both faces — do NOT
  switch this back to `Cross(E1,E2)` or one face renders black (see DEVLOG 2026-06-15 M9).
- ~1–2 frame readback latency (cosmetic).
- Floor/world collision: enable `bGroundPlane` (built-in plane), add a sphere/capsule slot, or
  `bUseDistanceFieldCollision`.
- Self-collision is point-particle repulsion → robust but not guaranteed clip-free (no CCD). Raise
  `SelfCollisionScale`(≈1)/`SelfCollisionStiffness`/`SelfCollisionIterations`/`Substeps` for tighter folds.
- Baked at `BeginPlay` (need a replay to change): constraint topology (`bUseBending`/`BendStiffness`),
  cloth `Orientation`, grid params. `SolverMode`, `Stiffness`, and self-collision params are live.

## Important Files
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimCompute.cpp` — shader classes + RDG dispatch (Jacobi/GS/self-collision branches).
- `Plugins/ClothSim/Shaders/Private/ClothPredict|ClothSolveDistance|ClothSolveGaussSeidel|ClothCollision|ClothCollisionDF|ClothBuildGrid|ClothSelfCollision|ClothFinalize.usf` — sim.
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSceneViewExtension.cpp` — GDF snapshot (M6).
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimComponent.cpp` — component, topology, constraint build+coloring, normals, update.
- `Plugins/ClothSim/Source/ClothSim/Private/ClothMeshSceneProxy.cpp` — mesh rendering.
- `Plugins/ClothSim/Source/ClothSim/Public/ClothSimResources.h` — `FClothSimParams`, `FClothRenderResources`, `FGPUConstraint`, `FClothColorRange`.
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimModule.cpp` — shader dir mapping.
- `Docs/` — all project documentation.

## How to Build & Run
1. **Close any ClothSimDemo editor** (Live Coding globally locks builds).
2. CLI build:
   `"E:/Epic Games/UE_5.7/Engine/Build/BatchFiles/Build.bat" ClothSimDemoEditor Win64 Development -Project="E:/ClaudeCode/RT_ClothSim/ClothSimDemo.uproject" -WaitMutex`
3. Open `ClothSimDemo.uproject`, drag a **ClothSimActor** into the level, Play.
   (Editor `.uproject` auto-build also works once it's not Live-Coding-locked.)
- Header/UPROPERTY/new-shader changes need a full rebuild + editor restart (Live Coding can't hot-patch them).

## Distance-Field Collision Notes (M6)
- Needs "Generate Mesh Distance Fields" (Project Settings) + a GDF consumer; forced via
  `r.DistanceFieldAO=1`/`r.AOGlobalDistanceField=1` in DefaultEngine.ini.
- Enable per-cloth with `bUseDistanceFieldCollision`. On-screen `ClothSim GDF: valid=/clipmaps=`
  reports whether the GDF snapshot is reaching the shader (green = good).
- The GDF is renderer-owned: `FClothSceneViewExtension::PostRenderBasePassDeferred_RenderThread`
  snapshots it. The DF shader binds a View UB only to satisfy transitive `ResolvedView` refs; GDF
  inputs come from `FGlobalDistanceFieldParameters2` (standalone/non-material path).

## Gauss-Seidel + Bending Notes (M7)
- `UClothSimComponent::BuildConstraints` (game thread, at `BeginPlay`) emits structural/shear/
  bending edges, greedily graph-colors them, and sorts them into a color-contiguous
  `FGPUConstraint` buffer + `FClothColorRange[]`. Uploaded once to a persistent pooled buffer.
- `ClothSolveGaussSeidel.usf` = one thread per constraint; the dispatcher loops
  `Iterations × Colors`, one pass per color. Race-free within a color; RDG serializes colors.
- `SolverMode` (Details → ClothSim|Solver) switches Jacobi ↔ Gauss-Seidel live. Bending only
  exists in the GS path; tune with `bUseBending` + `BendStiffness` (relative, ×global Stiffness).
- **To verify in-editor:** Play, set `SolverMode = Gauss-Seidel`; with fewer `SolverIterations`
  (e.g. 2–4) the cloth should hold its shape better than Jacobi at the same count. Toggle
  `bUseBending` and fold the cloth to see crease resistance.

## Debug Viz + Profiling Notes (M8)
- `bVisualizeStrain` (+ `StrainScale`) colors the cloth by membrane stretch (blue=compressed,
  green=rest, red=stretched). Written to the mesh vertex colors (needs a Vertex Color→BaseColor
  material to show on the lit surface) AND to the debug points (visible regardless of material).
- `bShowStats` shows solver mode, particle/constraint/color counts, and solve dispatches/substep.
- Profiling: use `ProfileGPU` (Ctrl+Shift+,), RenderDoc, or Unreal Insights — the per-pass
  `RDG_EVENT_NAME`s (`ClothSolveDistance` / `ClothSolveGaussSeidel (… color N)` etc.) are the
  timing units. **Do NOT** add `RDG_GPU_STAT_SCOPE`/`RDG_EVENT_SCOPE` to `Dispatch_RenderThread`:
  those push RHI breadcrumbs that are unbalanced on this standalone RDG builder and crash at
  `Execute()` (breadcrumb-sentinel assertion). Learned the hard way — see DEVLOG 2026-06-15 M8.

## Self-Collision Notes (M9)
- `ClothBuildGrid.usf` bins particles into a spatial hash grid (cell = thickness; `InterlockedAdd`
  bucket append — the only atomics in the project); `ClothSelfCollision.usf` scans the 27 neighbour
  cells and repels close non-1-ring particles (race-free Jacobi gather, ping-pongs `PredictedA/B`).
  Looped `SelfCollisionIterations`×/substep, rebuilding the grid each iteration.
- Enable `bSelfCollision`; tune `SelfCollisionScale` (×Spacing, ≈1 = continuous barrier),
  `SelfCollisionStiffness`, `SelfCollisionIterations`; raise `Substeps` to cut tunnelling.
- `bDebugSelfCollision` = CPU overlap check from the readback: red points on contacting particles +
  on-screen contact count. (Capped to ≤5000 particles.)
- **Drop test:** `Orientation = Horizontal sheet` + unpin + `bGroundPlane` (set `GroundHeight`) →
  cloth falls flat. Fold it (drape over a sphere, or wind) to exercise self-collision.
- Point-particle repulsion only — not clip-proof. Continuous (vertex-triangle/edge-edge) CCD is the
  stretch upgrade.

## Immediate Next Task
**M1–M9 complete** (full sim + rendering + wind + all collision types + two solvers + debug/profiling).
Remaining work is optional/stretch:
- **M+ — Zero-copy GPU rendering:** compute writes UAV vertex buffers directly (custom vertex
  factory / vertex pulling), removing the readback → CPU → lock+memcpy round trip.
- **M+ — Continuous self-collision (CCD):** vertex-triangle / edge-edge for guaranteed clip-free contact.
- **Solver depth:** XPBD compliance per constraint; true dihedral bending instead of 2-away distance.
- **Portfolio captures:** Jacobi-vs-Gauss-Seidel convergence/ms + resolution-vs-ms graph (M8 hooks).

## Recommended Prompt For Future Claude Sessions
> "Read `Docs/HANDOFF.md`, `Docs/PROJECT_STATE.md`, and `Docs/ARCHITECTURE.md` to load context.
> This is a from-scratch GPU cloth sim in UE 5.7 (plugin `ClothSim`). M1–M9 are done. Continue
> with the milestone listed under 'Immediate Next Task', following the workflow in
> `Docs/` (update PROJECT_STATE, DEVLOG, ROADMAP, HANDOFF, PORTFOLIO_NOTES + commit per
> milestone). Build via the CLI command in HANDOFF; close the editor first (Live Coding lock).
> IMPORTANT: do not commit/update docs until the user verifies the milestone works in-editor."
