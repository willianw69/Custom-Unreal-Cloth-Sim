# PROJECT_STATE.md

> Single source of truth for current project status. Update after every milestone.
> Last updated: 2026-06-12 (after M4).

## Project Overview
Real-time **GPU cloth simulation built from scratch** in **Unreal Engine 5.7**, as a
Technical Artist portfolio piece. The cloth is simulated entirely with **custom compute
shaders** (no Chaos Cloth). Position/velocity live in GPU structured buffers, constraints
are solved on the GPU (XPBD/PBD), and the result is rendered as a dynamic lit mesh.

- **Host project:** `ClothSimDemo` (UE 5.7 C++).
- **Plugin (all the real work):** `Plugins/ClothSim`.
- **Engine install:** `E:\Epic Games\UE_5.7`.

## Current Milestone
**M4 — Wind Forces: COMPLETE and verified in-editor.**

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

## In-Progress Work
- None (between milestones).

## Next Milestone
**M5 — Sphere & Capsule Collision.** GPU collider buffer; a collision pass projects predicted
positions out of spheres/capsules and applies tangential friction so the cloth drapes over
objects instead of passing through.

## Technical Decisions
- **Wind model:** normal-dependent aerodynamic drag computed in the Predict pass; per-particle
  normals derived on the GPU from grid neighbours (no extra pass); sinusoidal turbulence.
- **Solver model:** XPBD/PBD. Velocity is derived from the position delta (stable).
- **Parallelism:** Jacobi via **per-particle gather** of grid neighbours (no atomics, no
  races). Graph-colored Gauss-Seidel deferred to M6.
- **Timestep:** fixed-step accumulator (default 1/60 s) → frame-rate independent.
- **Rendering:** reliable path = `FLocalVertexFactory` updated from a small CPU readback
  (negligible at current vertex counts). Zero-copy GPU vertex write is a planned upgrade.
- **Spaces:** sim runs in **world space**; positions converted to local for the mesh proxy.

## Known Issues
- Default material is **one-sided** → cloth invisible from the back. Use a Two-Sided material.
- Rendering lags the true GPU state by ~1–2 frames (readback latency). Cosmetic only.

## Known Limitations
- No collision yet → cloth falls through the floor (M5).
- No bending constraints → cloth can fold sharply (M6).
- Solver assumes a **regular grid** (neighbours computed from grid coords), not an arbitrary mesh.
- Normals computed on CPU (M3 reliable path), not GPU.
- Rendering is readback-based, not zero-copy.

## Future Improvements
- Zero-copy GPU vertex write (compute → UAV vertex buffers, custom vertex factory).
- Graph-colored Gauss-Seidel solver (faster convergence) + bending/dihedral constraints.
- GPU normals pass.
- Sphere/capsule (M5) and self-collision.
- Profiling pass with Unreal Insights / `stat GPU` capture for the portfolio.
