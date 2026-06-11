# HANDOFF.md

> For a new session/engineer to continue immediately. Assume zero prior context.
> Update after every milestone — always represents the current state.
> Last updated: 2026-06-11 (after M3).

## Project Summary
From-scratch **GPU cloth simulation in UE 5.7** (no Chaos Cloth). Custom compute shaders do
XPBD/PBD on structured buffers; rendered as a dynamic lit mesh. Portfolio project for a TA.
Host project `ClothSimDemo`, all work in `Plugins/ClothSim`. Engine: `E:\Epic Games\UE_5.7`.

## Current Milestone
M3 (Cloth Rendering) complete and verified. Next is M4 (Wind Forces).

## Completed Work
- M1: GPU integration + structured buffers + RDG + debug points.
- M2: XPBD distance constraints (Jacobi per-particle gather) + substepping + fixed timestep.
- M3: `UMeshComponent` + `FClothMeshSceneProxy` (FLocalVertexFactory), lit cloth, CPU normals.

## Current Technical Decisions
- PBD/XPBD; velocity derived from position delta.
- Solver = Jacobi per-particle gather of grid neighbours (no atomics). Gauss-Seidel later (M6).
- Fixed-timestep accumulator (1/60 s) for frame-rate independence.
- Sim in world space; converted to local for the mesh.
- Rendering via readback → FLocalVertexFactory (reliable). Zero-copy is a future upgrade.

## Known Issues
- Default material one-sided → use a Two-Sided material to see both faces.
- ~1–2 frame readback latency (cosmetic).
- No collision yet → falls through floor (M5).

## Important Files
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimCompute.cpp` — shader classes + RDG dispatch.
- `Plugins/ClothSim/Shaders/Private/ClothPredict|ClothSolveDistance|ClothFinalize.usf` — sim.
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimComponent.cpp` — component, topology, normals, update.
- `Plugins/ClothSim/Source/ClothSim/Private/ClothMeshSceneProxy.cpp` — mesh rendering.
- `Plugins/ClothSim/Source/ClothSim/Public/ClothSimResources.h` — `FClothSimParams`, `FClothRenderResources`.
- `Plugins/ClothSim/Source/ClothSim/Private/ClothSimModule.cpp` — shader dir mapping.
- `Docs/` — all project documentation.

## How to Build & Run
1. **Close any ClothSimDemo editor** (Live Coding globally locks builds).
2. CLI build:
   `"E:/Epic Games/UE_5.7/Engine/Build/BatchFiles/Build.bat" ClothSimDemoEditor Win64 Development -Project="E:/ClaudeCode/RT_ClothSim/ClothSimDemo.uproject" -WaitMutex`
3. Open `ClothSimDemo.uproject`, drag a **ClothSimActor** into the level, Play.
   (Editor `.uproject` auto-build also works once it's not Live-Coding-locked.)
- Header/UPROPERTY/new-shader changes need a full rebuild + editor restart (Live Coding can't hot-patch them).

## Immediate Next Task
**M4 — Wind Forces.** Add wind params (direction, strength, turbulence) to `FClothSimParams`
and `UClothSimComponent`; apply an aerodynamic drag force in `ClothPredict.usf`
(`F ∝ (v_rel·n) n`, needs per-particle normal — reuse grid normal or pass from a normals pass).

## Recommended Prompt For Future Claude Sessions
> "Read `Docs/HANDOFF.md`, `Docs/PROJECT_STATE.md`, and `Docs/ARCHITECTURE.md` to load context.
> This is a from-scratch GPU cloth sim in UE 5.7 (plugin `ClothSim`). M1–M3 are done. Continue
> with the milestone listed under 'Immediate Next Task', following the workflow in
> `Docs/` (update PROJECT_STATE, DEVLOG, ROADMAP, HANDOFF, PORTFOLIO_NOTES + commit per
> milestone). Build via the CLI command in HANDOFF; close the editor first (Live Coding lock)."
