# PORTFOLIO_NOTES.md

> Portfolio-worthy accomplishments per milestone. Source material for portfolio breakdowns,
> resume bullets, and interview prep.

---

## M1 — GPU Particle Simulation Infrastructure
**Technical challenges:** Standing up a custom compute-shader pipeline in UE5 from scratch:
virtual shader path mapping at the correct module load phase, persistent pooled GPU buffers
that survive across frames, and the strict game-thread/render-thread split.
**Technologies:** UE 5.7, HLSL compute shaders, RDG (Render Dependency Graph), `FGlobalShader`,
`FRHIGPUBufferReadback`, structured buffers.
**Implementation:** `Positions`/`Velocities`/`InvMasses` structured buffers; an integration
compute shader; RDG dispatch enqueued from the game thread; non-stalling readback for debug.

## M2 — XPBD Constraint Solver on the GPU
**Technical challenges:** Solving distance constraints in parallel without data races. Chose a
**per-particle gather** (each thread writes only its own particle, reads a fixed snapshot) →
pure Jacobi, no atomics. Diagnosed and fixed a **frame-rate-dependent instability** (energy
injection from `v=(p−x)/dt` under variable `dt`) with a fixed-timestep accumulator.
**Technologies:** Position Based Dynamics / XPBD, Jacobi relaxation, substepping, HLSL.
**Implementation:** Predict → Solve(×N, ping-pong buffers) → Finalize; structural + shear
constraints; velocity derived from solved positions.

## M3 — Dynamic Cloth Mesh Rendering
**Technical challenges:** Rendering GPU-driven deforming geometry through UE's render pipeline —
custom `FPrimitiveSceneProxy`, `FLocalVertexFactory`, per-frame vertex updates, smooth normal
generation, correct local/world space handling, and navigating 5.7 RHI API changes.
**Technologies:** `UMeshComponent`, `FPrimitiveSceneProxy`, `FLocalVertexFactory`,
`FStaticMeshVertexBuffers`, UE material system.
**Implementation:** Static grid topology + UVs; per-frame readback → local conversion →
area-weighted normals/tangents → proxy buffer upload; standard lit mesh batch.

## M4 — Wind & Aerodynamic Drag
**Technical challenges:** Making wind look physical rather than a uniform shove — the force must
depend on each face's orientation. Computed per-particle surface normals on the GPU inside the
prediction pass (no extra pass/buffer) and applied a normal-projected drag force. Added animated
turbulence for organic gusting.
**Technologies:** HLSL compute, aerodynamic drag model, procedural turbulence (sum of sines).
**Implementation:** `F_aero = WindDrag · dot(v_air − v_cloth, n) · n` in `ClothPredict.usf`;
`v_air = WindDir·WindStrength + turbulence(pos, time)`; exposed wind params on the component.

## M5 — Sphere & Capsule Collision + Friction
**Technical challenges:** GPU collision against analytic shapes with correct response and
friction, ordered correctly within the PBD substep, without data races. Unified spheres and
capsules as a single segment+radius primitive to keep the kernel branch-light.
**Technologies:** HLSL compute, closest-point-on-segment, positional projection, Coulomb-style
tangential friction, structured collider buffer.
**Implementation:** `ClothCollision.usf` runs after the solver, in place on the predicted buffer
(one thread per particle → no races); pushes penetrating particles to the surface and damps
tangential motion. Colliders authored as Details-panel slots, converted to world space per frame.

## M6 — Distance Field Collision Against Arbitrary Meshes
**Technical challenges:** Hooking into Unreal's renderer-owned **Global Distance Field** from a
custom plugin — the same mechanism Niagara uses for GPU particle collision. Required a
SceneViewExtension to snapshot the GDF at the right point in the frame, sampling in translated
world space, and resolving a non-obvious dependency where the engine's GDF shader header
transitively requires the View uniform buffer.
**Technologies:** UE Global Distance Field, `FSceneViewExtension`, `FXRenderingUtils`,
`FGlobalDistanceFieldParameters2`, signed distance field sampling + gradient.
**Implementation:** `FClothSceneViewExtension` caches GDF params + view UB in
`PostRenderBasePassDeferred`; `ClothCollisionDF.usf` samples `GetDistanceToNearestSurfaceGlobal`
and the gradient to push particles out of any scene mesh, with tangential friction.

## M7 — Graph-Colored Gauss-Seidel Solver + Bending
**Technical challenges:** Running **Gauss-Seidel** (not Jacobi) distance constraints in parallel on
the GPU — which is inherently sequential — without data races. Solved it with **CPU graph coloring**:
partition constraints so no two in a color share a particle, then dispatch one color at a time;
within a color all writes are disjoint, and across colors RDG serializes the read-modify-write so
later corrections build on earlier ones (true Gauss-Seidel propagation, faster convergence than the
Jacobi gather's under-relaxed averaging). Moved from an implicit grid-neighbour solver to an
**explicit constraint buffer**, which both generalizes the topology and provides the natural home
for bending constraints (2-away distance) that resist creasing. Kept the Jacobi path live behind a
runtime toggle for direct comparison.
**Technologies:** Graph coloring, parallel Gauss-Seidel, Position Based Dynamics, HLSL compute,
RDG UAV serialization, one-thread-per-constraint dispatch.
**Implementation:** `BuildConstraints()` emits structural/shear/bending edges + greedy coloring →
color-sorted `FGPUConstraint` buffer + `[Start,Count)` ranges; `ClothSolveGaussSeidel.usf` projects
both endpoints of each constraint in place; the dispatcher loops `Iterations × Colors`.
`EClothSolverMode` switches Jacobi ↔ Gauss-Seidel at runtime.

## M8 — Strain Visualization + Profiling
**Technical challenges:** Making the simulation legible. Computed per-particle **membrane strain**
(signed deviation of structural-neighbour edge lengths from rest) and mapped it to a color ramp,
driving both the mesh's per-vertex color buffer (a new per-frame upload path) and the debug points
so it reads with or without a vertex-color material. Surfaced solver cost with an on-screen stats
readout and per-pass GPU profiling labels. Also debugged a **renderer crash**: wrapping the
standalone RDG builder in `RDG_GPU_STAT_SCOPE`/`RDG_EVENT_SCOPE` pushed RHI breadcrumbs that were
unbalanced at `Execute()` (the builder runs from a render command on the immediate list, outside
the renderer's managed breadcrumb scope) — diagnosed from the crash callstack and resolved by
relying on per-pass `RDG_EVENT_NAME` labels instead.
**Technologies:** `FColorVertexBuffer` dynamic update, color-ramp strain mapping, RDG pass events,
`ProfileGPU`/RenderDoc/Insights, on-screen debug HUD.
**Implementation:** `ComputeStrainColors()` (CPU, from the position readback) → vertex colors +
debug points; `bShowStats` HUD reports solver mode + particle/constraint/color/dispatch counts;
named passes give per-pass timings for the Jacobi-vs-Gauss-Seidel comparison.

---

## M9 — GPU Self-Collision (spatial hash) + Two-Sided Shading Fix
**Technical challenges:** Cloth-vs-itself collision on the GPU. Built a **uniform spatial hash grid**
broadphase (atomic bucket append) so each particle only tests its 27 neighbour cells (O(N·avgPerCell),
not O(N²)), with a **race-free Jacobi-gather repulsion** as the response and graph-friendly ping-pong
buffers. Confined the only atomics in the project to the broadphase build. Tuned for robustness
(thickness ≈ spacing so repulsion spheres form a continuous barrier, iterations, substeps) and was
candid about the limitation: point-particle repulsion isn't continuous collision, so guaranteed
clip-free contact would need vertex-triangle/edge-edge CCD. Added supporting features (cloth
orientation, a built-in ground plane, a CPU self-collision debug overlay) to make a clean drop test.
**Also debugged a subtle two-sided-shading bug:** a face rendering pure black under any light because
the smooth normals (`Cross(E1,E2)`, right-handed) disagreed with UE's **left-handed winding**, which
a two-sided material uses to flip the normal — so the visible face was lit with an inward normal.
**Technologies:** spatial hashing, GPU atomics (`InterlockedAdd`), typed vs structured RDG buffers,
`AddClearUAVPass`, PBD inequality constraints, two-sided material shading / winding conventions.
**Implementation:** `ClothBuildGrid.usf` + `ClothSelfCollision.usf`; iteration loop in the dispatcher;
`EClothOrientation`; ground plane folded into `ClothCollision.usf`; normal fix = `Cross(E2,E1)`.

## Portfolio Talking Points
- Built a real-time **GPU cloth simulator from scratch** in UE5 — no Chaos Cloth — demonstrating
  both physics-simulation and rendering-pipeline understanding.
- Designed the full pipeline: compute simulation (RDG) → readback → dynamic lit mesh rendering.
- Made deliberate, defensible engineering trade-offs (Jacobi vs Gauss-Seidel; readback vs
  zero-copy) and documented them.

## Resume Bullet Candidates
- "Implemented a real-time GPU cloth simulation in Unreal Engine 5.7 using custom HLSL compute
  shaders and the Render Dependency Graph, solving XPBD distance constraints on the GPU."
- "Built a custom `FPrimitiveSceneProxy`/`FLocalVertexFactory` rendering path for GPU-driven
  deforming meshes with runtime normal generation."
- "Diagnosed and fixed a frame-rate-dependent simulation instability via fixed-timestep
  substepping, achieving stable, deterministic cloth behaviour."
- "Added GPU wind interaction with a normal-dependent aerodynamic drag model and procedural
  turbulence, producing realistic billowing without an extra simulation pass."
- "Implemented GPU sphere/capsule collision with positional correction and tangential friction,
  unifying both shapes as a segment+radius primitive for a branch-light kernel."
- "Integrated Unreal's Global Distance Field into a custom compute pass (via a SceneViewExtension)
  so GPU cloth collides against arbitrary scene meshes — the technique Niagara uses for particle
  collision."
- "Implemented a graph-colored parallel Gauss-Seidel constraint solver on the GPU — CPU coloring
  guarantees race-free in-place projection per color while RDG serialization preserves Gauss-Seidel
  ordering — plus bending constraints, with the Jacobi solver retained behind a runtime toggle for
  convergence comparison."
- "Added real-time strain visualization (per-vertex stretch → color ramp) and GPU profiling
  instrumentation, and diagnosed an RHI-breadcrumb renderer crash from a crash-dump callstack."
- "Implemented GPU cloth self-collision with a uniform spatial-hash broadphase (atomic bucket append)
  and a race-free Jacobi repulsion response, reducing pair testing from O(N²) to O(N·avg-per-cell)."
- "Diagnosed a two-sided shading bug where smooth normals (right-handed cross product) disagreed with
  Unreal's left-handed winding convention, causing a black face under all lighting."

## Interview Discussion Points
- Why PBD/XPBD over mass-spring; why velocity is derived from position deltas (stability).
- The parallel-solve data race and why per-particle gather (Jacobi) avoids it without atomics;
  the convergence trade-off vs graph-colored Gauss-Seidel — and how graph coloring + RDG UAV
  serialization makes Gauss-Seidel safe AND parallel (disjoint writes per color, ordered across
  colors).
- Why bending constraints (2-away distance) are needed and where they sit in the constraint graph;
  per-constraint relative stiffness vs a single global stiffness.
- How GPU breadcrumbs/draw-event scopes are tracked on the RHI command list, and why opening one on
  a plugin's standalone RDG builder (run from a render command) imbalances the stack — a concrete
  example of reading a crash callstack to root-cause a renderer assertion.
- Spatial hashing for GPU broadphase: why atomics are acceptable in the build but the response stays
  Jacobi/race-free; and why point-particle self-collision can't guarantee clip-free contact the way
  vertex-triangle/edge-edge CCD can.
- Coordinate-system handedness and two-sided shading: how a right-handed normal vs. Unreal's
  left-handed winding silently breaks lit rendering only for two-sided materials.
- Why fixed timestep matters for deterministic, frame-rate-independent physics.
- CPU/GPU threading model in UE (game vs render thread, RDG, pooled vs transient buffers).
- Readback vs zero-copy vertex rendering trade-offs.
