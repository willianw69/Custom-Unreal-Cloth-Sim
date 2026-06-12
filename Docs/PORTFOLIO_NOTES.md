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

---

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

## Interview Discussion Points
- Why PBD/XPBD over mass-spring; why velocity is derived from position deltas (stability).
- The parallel-solve data race and why per-particle gather (Jacobi) avoids it without atomics;
  the convergence trade-off vs graph-colored Gauss-Seidel.
- Why fixed timestep matters for deterministic, frame-rate-independent physics.
- CPU/GPU threading model in UE (game vs render thread, RDG, pooled vs transient buffers).
- Readback vs zero-copy vertex rendering trade-offs.
