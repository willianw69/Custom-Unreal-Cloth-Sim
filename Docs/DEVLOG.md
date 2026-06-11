# DEVLOG.md

> Chronological development history. Append new entries; never replace old ones.

---

## 2026-06-11 — Project bootstrap + M1: Particle Simulation
**What:** Created host project `ClothSimDemo` (UE 5.7) and the `ClothSim` plugin. Implemented
GPU particle integration: `Positions`/`Velocities`/`InvMasses` structured buffers, a
`ClothIntegrate` compute shader (gravity + damping), RDG dispatch on the render thread, and
debug-point visualization via a non-stalling `FRHIGPUBufferReadback`.

**Why:** Establish and de-risk the hardest infrastructure (shader directory mapping, pooled
buffers, game↔render threading, RDG compute) before adding physics.

**Problems & solutions:**
- Targets used `BuildSettingsVersion.V5` → UE 5.7 requires **V6**. Fixed in both `.Target.cs`.
- `TUniquePtr<FRHIGPUBufferReadback>` with a forward-declared type caused **C4150** (delete of
  incomplete type) when `MakeShared` instantiated the dtor in the component TU. Fixed by
  declaring `FClothRenderResources`'s ctor+dtor **out-of-line** in the .cpp that has the full type.
- Crash: "Couldn't find source file of virtual shader path '/ClothSim/ClothIntegrate.usf'".
  The mapping `/ClothSim → Shaders/`, but the file is in `Shaders/Private/`. Fixed the
  `IMPLEMENT_GLOBAL_SHADER` path to **`/ClothSim/Private/ClothIntegrate.usf`**.
- Build lock: Live Coding from *any* open editor (even other projects) globally blocks CLI/VS
  builds. Workflow: close the ClothSimDemo editor before building.

**Performance:** Trivial (1024 particles, single dispatch). Named pass visible in `stat GPU`.

**Next:** M2 distance constraints.

---

## 2026-06-11 — M2: XPBD Distance Constraints (+ fixed-timestep fix)
**What:** Replaced single integration with **Predict → Solve(×N) → Finalize** and substepping.
Solver = **Jacobi per-particle gather**: each thread reads up to 8 grid neighbours (4 structural
rest=`Spacing`, 4 shear rest=`Spacing·√2`), computes PBD distance corrections, averages them,
writes its own slot. Two predicted buffers ping-pong across iterations. Velocity is derived in
Finalize as `(predicted − x)/dt`. Removed `ClothIntegrate.usf`.

**Why:** Turn loose particles into cloth. Per-particle gather avoids the read/write race of
per-constraint solving with no atomics — the cleanest correct GPU solver for a regular grid.

**Problems & solutions:**
- **Cloth lurched up/down, correlated with mouse movement.** Root cause: the editor varies
  frame rate with interaction → variable `dt`; `v=(p−x)/dt` injected energy on small `dt`.
  Fixed with a **fixed-timestep accumulator** (default 1/60 s, capped steps/frame) so the sim
  advances in constant chunks regardless of render FPS. Confirmed stable.

**Performance:** Default 2 substeps × 8 iterations = ~20 passes/frame at 1024 particles, no
measurable cost. Iterations trade stiffness vs. time; substeps trade stability vs. time.

**Next:** M3 rendering.

---

## 2026-06-11 — M3: Cloth Rendering
**What:** Converted `UClothSimComponent` from `USceneComponent` to **`UMeshComponent`** and added
**`FClothMeshSceneProxy`** (modeled on engine `FProceduralMeshSceneProxy`): single section,
`FStaticMeshVertexBuffers` + `FLocalVertexFactory` + `FDynamicMeshIndexBuffer32`. Static grid
topology/UVs built once. Each frame: read GPU positions from readback → convert to local space
→ compute smooth area-weighted normals + tangents (CPU) → push to proxy, which lock+memcpys the
position and tangent buffers. Material exposed via `ClothMaterial`.

**Why:** Render real lit fabric instead of points.

**Decisions & problems:**
- Investigated **zero-copy** (compute writes UAV vertex buffers). Found the engine's vertex
  buffer helpers update via CPU lock+memcpy, and 5.7 deprecated the simple `CreateUAV(buffer,
  format)` overloads (now `FRHIViewDesc` builders). Chose the **reliable readback-based** path;
  zero-copy is a documented future upgrade. Readback is negligible at this vertex count.
- 5.7 API notes: use `GetMaterialRelevance(EShaderPlatform)` (the `ERHIFeatureLevel` overload is
  deprecated); `FStaticMeshVertexBuffers::InitFromDynamicVertex(VF, Verts, NumTexCoords)`.
- Compiled first try after pre-checking signatures against the installed engine source.

**Performance:** Mesh draw is a standard `FLocalVertexFactory` batch. CPU normals + readback copy
are O(verts), negligible at 1024.

**Known caveat:** default material is one-sided → assign a Two-Sided material for full visibility.

**Next:** M4 wind forces.

---

## 2026-06-11 — Docs/Workflow setup
**What:** Added `Docs/` (PROJECT_STATE, ARCHITECTURE, ROADMAP, DEVLOG, HANDOFF, PORTFOLIO_NOTES),
a UE `.gitignore`, and initialized git per the Development Workflow Requirements. Backfilled
M1–M3 history. Adopting per-milestone doc updates + commits going forward.

**Next:** M4 wind forces.
