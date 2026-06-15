# HANDOFF.md

> For a new session/engineer to continue immediately. Assume zero prior context.
> Update after every milestone — always represents the current state.
> Last updated: 2026-06-15 (after M8).

## Project Summary
From-scratch **GPU cloth simulation in UE 5.7** (no Chaos Cloth). Custom compute shaders do
XPBD/PBD on structured buffers; rendered as a dynamic lit mesh. Portfolio project for a TA.
Host project `ClothSimDemo`, all work in `Plugins/ClothSim`. Engine: `E:\Epic Games\UE_5.7`.

## Current Milestone
M8 (Debug Viz Polish + Profiling) complete and verified in-editor. **Core roadmap M1–M8 is done.**
Remaining work is stretch goals (M+: zero-copy rendering; or deeper solver work).

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
- Default material one-sided → use a Two-Sided material to see both faces.
- ~1–2 frame readback latency (cosmetic).
- Floor/world collision = add a large sphere/capsule slot or enable `bUseDistanceFieldCollision`.
- Constraint topology (incl. `bUseBending`/`BendStiffness`) is baked at `BeginPlay` → changing it
  needs a replay. `SolverMode` and `Stiffness` are live.

## Important Files
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimCompute.cpp` — shader classes + RDG dispatch (Jacobi & GS branches).
- `Plugins/ClothSim/Shaders/Private/ClothPredict|ClothSolveDistance|ClothSolveGaussSeidel|ClothCollision|ClothCollisionDF|ClothFinalize.usf` — sim.
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

## Immediate Next Task
**Core roadmap (M1–M8) is complete.** Remaining work is optional/stretch:
- **M+ — Zero-copy GPU rendering:** compute writes UAV vertex buffers directly (custom vertex
  factory / vertex pulling), removing the readback → CPU → lock+memcpy round trip.
- **Solver depth:** XPBD compliance per constraint (instead of clamped PBD stiffness); true
  dihedral bending instead of the 2-away distance approximation.
- **Portfolio captures:** record the Jacobi-vs-Gauss-Seidel convergence/ms comparison and a
  resolution-vs-ms graph using the M8 hooks (interactive, in-editor).

## Recommended Prompt For Future Claude Sessions
> "Read `Docs/HANDOFF.md`, `Docs/PROJECT_STATE.md`, and `Docs/ARCHITECTURE.md` to load context.
> This is a from-scratch GPU cloth sim in UE 5.7 (plugin `ClothSim`). M1–M8 are done. Continue
> with the milestone listed under 'Immediate Next Task', following the workflow in
> `Docs/` (update PROJECT_STATE, DEVLOG, ROADMAP, HANDOFF, PORTFOLIO_NOTES + commit per
> milestone). Build via the CLI command in HANDOFF; close the editor first (Live Coding lock)."
