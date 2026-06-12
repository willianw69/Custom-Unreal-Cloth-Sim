# ROADMAP.md

> Project progress tracker. ✅ Completed · 🔄 In Progress · ⏳ Planned
> Last updated: 2026-06-12 (after M5).

| Milestone | Title | Status |
|---|---|---|
| M1 | Particle Simulation | ✅ |
| M2 | XPBD Distance Constraints | ✅ |
| M3 | Cloth Rendering | ✅ |
| M4 | Wind Forces | ✅ |
| M5 | Sphere & Capsule Collision | ✅ |
| M6 | Colored Gauss-Seidel + Bending | ⏳ |
| M7 | Debug Viz Polish + Profiling | ⏳ |
| M+ | Zero-copy GPU Rendering Path | ⏳ (stretch) |

## Notes per Milestone

### M1 — Particle Simulation ✅
GPU integration (gravity + damping) into structured buffers via RDG; debug-point render
through a non-stalling readback. Proved the hard infrastructure: shader dir mapping, pooled
buffers, game↔render threading, compute dispatch.

### M2 — XPBD Distance Constraints ✅
Predict/Solve/Finalize pipeline with substepping. Solver = Jacobi per-particle gather of
structural + shear constraints. Added fixed-timestep accumulator to remove frame-rate
dependence (a real bug found during testing). Cloth hangs like a curtain.

### M3 — Cloth Rendering ✅
Converted component to `UMeshComponent`; custom `FClothMeshSceneProxy` with
`FLocalVertexFactory`. Lit fabric surface, CPU smooth normals, vertices from GPU sim via
small readback. (Zero-copy GPU vertex write intentionally deferred.)

### M4 — Wind Forces ✅
World-space wind vector + animated turbulence; normal-dependent aerodynamic drag
`F = WindDrag · dot(v_air−v_cloth, n) · n` applied in the Predict pass, with per-particle GPU
normals from grid neighbours. Exposed WindDirection/Strength/Drag/Turbulence. Cloth billows.

### M5 — Sphere & Capsule Collision ✅
Collider list in a GPU buffer (unified capsule = segment+radius; sphere = degenerate).
`ClothCollision.usf` pass after the solver projects penetrating particles to the surface and
damps tangential motion (friction). Editable collider slots + `bPinTopEdge` on the component.

### M6 — Colored Gauss-Seidel + Bending ⏳
Explicit constraint buffer + CPU graph coloring → parallel Gauss-Seidel (faster convergence).
Add bending/dihedral constraints to resist sharp folds.

### M7 — Debug Viz Polish + Profiling ⏳
GPU-resident particle/constraint debug draws (strain coloring), Unreal Insights / `stat GPU`
captures, resolution-vs-ms graph for the portfolio.

### M+ — Zero-copy GPU Rendering Path ⏳ (stretch)
Replace the readback-based mesh update with compute writing directly into UAV vertex buffers
(custom vertex factory / vertex pulling). Purest "GPU all the way" portfolio result.
