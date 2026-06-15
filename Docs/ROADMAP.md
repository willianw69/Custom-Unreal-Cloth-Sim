# ROADMAP.md

> Project progress tracker. ✅ Completed · 🔄 In Progress · ⏳ Planned
> Last updated: 2026-06-15 (after M8).

| Milestone | Title | Status |
|---|---|---|
| M1 | Particle Simulation | ✅ |
| M2 | XPBD Distance Constraints | ✅ |
| M3 | Cloth Rendering | ✅ |
| M4 | Wind Forces | ✅ |
| M5 | Sphere & Capsule Collision | ✅ |
| M6 | Distance Field Mesh Collision | ✅ |
| M7 | Colored Gauss-Seidel + Bending | ✅ |
| M8 | Debug Viz Polish + Profiling | ✅ |
| M+ | Zero-copy GPU Rendering Path | ⏳ (stretch) |

(M6 was reassigned from the originally-planned solver upgrade to distance-field collision at the
user's request; the Gauss-Seidel + bending work is now M7.)

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

### M6 — Distance Field Mesh Collision ✅
Cloth collides with arbitrary scene meshes via Unreal's Global Distance Field. SceneViewExtension
snapshots GDF params + view uniform buffer (`PostRenderBasePassDeferred`); `ClothCollisionDF.usf`
samples the field + gradient to project particles out. Toggle `bUseDistanceFieldCollision`.
Requires "Generate Mesh Distance Fields" + a GDF consumer (forced via DFAO cvars).

### M7 — Colored Gauss-Seidel + Bending ✅
Explicit `FGPUConstraint` buffer (structural + shear + bending) + greedy CPU graph coloring →
`ClothSolveGaussSeidel.usf` runs one thread per constraint, one dispatch per color (race-free
in place; RDG serializes colors → true Gauss-Seidel). Bending = 2-away distance constraint with
relative `BendStiffness`. `EClothSolverMode` toggles Jacobi ↔ Gauss-Seidel at runtime; the M2
Jacobi gather is retained as the baseline for the M8 profiling comparison.

### M8 — Debug Viz Polish + Profiling ✅
Strain visualization (per-particle stretch → blue/green/red, on vertex colors + debug points,
`bVisualizeStrain`/`StrainScale`); on-screen stats readout (`bShowStats`: solver mode,
particle/constraint/color counts, solve dispatches/substep); per-pass `RDG_EVENT_NAME` profiling
visible in `ProfileGPU`/RenderDoc/Insights. (A dedicated `stat GPU` scope was attempted but removed
— RHI breadcrumb scopes are unsafe on this plugin's standalone RDG builder; see DEVLOG.)
Interactive captures + the resolution-vs-ms graph are left to the user with the hooks now in place.

### M+ — Zero-copy GPU Rendering Path ⏳ (stretch)
Replace the readback-based mesh update with compute writing directly into UAV vertex buffers
(custom vertex factory / vertex pulling). Purest "GPU all the way" portfolio result.
