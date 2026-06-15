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
M1–M3 history. Adopting per-milestone doc updates + commits going forward. Pushed to GitHub
(`willianw69/Custom-Unreal-Cloth-Sim`, branch `main`).

**Next:** M4 wind forces.

---

## 2026-06-12 — M4: Wind Forces
**What:** Added a normal-dependent aerodynamic wind force to `ClothPredict.usf`. Each particle
computes its smooth surface normal on the GPU from grid neighbours (central differences, no
extra pass/buffer), then applies `F_aero = WindDrag · dot(v_air − v_cloth, n) · n`. Air velocity
= `WindDirection·WindStrength` plus animated turbulence (sum of sines of position+time scaled by
`WindTurbulence`). New params on `FClothSimParams`, `FClothPredictCS`, and `UClothSimComponent`
(WindDirection/Strength/Drag/Turbulence). `TimeSeconds` sourced from world time.

**Why:** Wind interaction is a core requirement and a visually compelling portfolio feature.
A normal-dependent (vs uniform) force is what produces realistic billowing — faces angled into
the wind catch it; edge-on faces slip through.

**Problems & solutions:** None significant — compiled first try. Wind defaults to Strength=0 so
behaviour matches M3 until enabled. Verified billowing/rippling, stable, anchored at corners.

**Performance:** Predict pass now also reads 4 neighbours/particle for the normal; negligible at
1024 particles. No extra passes or buffers.

**Next:** M5 sphere & capsule collision.

---

## 2026-06-12 — M5: Sphere & Capsule Collision
**What:** Added `ClothCollision.usf`, a pass that runs after the distance solver each substep
(before Finalize) and projects predicted positions out of colliders, plus tangential friction.
Colliders are unified as a **capsule (segment A-B + radius)**; a **sphere is the A==B** case, so
one routine handles both. Collider slots authored in the Details panel
(`TArray<FClothCollider>`: type, center, radius, half-height, rotation) are converted to
world-space `FGPUCollider`s each frame and uploaded to a structured buffer. Added a global
`Friction` and a `bPinTopEdge` option for clean draping tests.

**Why:** Collision is a core requirement and the feature that makes the cloth interact with the
world (drape over a sphere, slide off a capsule).

**Problems & solutions:**
- Pass ordering: collision runs **after** Solve and **before** Finalize, operating on the solved
  predicted buffer **in place** (each thread touches only its own particle → bound the predicted
  buffer as a UAV with no races). Friction uses `PrevPositions` = the start-of-substep `Positions`
  (still valid at this point since Finalize hasn't run yet). RDG sequences the UAV-write →
  SRV-read into Finalize automatically.
- Used a 1 cm `ContactOffset` skin so the cloth rests just off the surface (avoids z-fighting/poke).

**Performance:** O(particles × colliders) per substep; trivial for a handful of colliders.

**Next:** M6 — colored Gauss-Seidel solver + bending constraints.

---

## 2026-06-12 — M6: Distance Field Mesh Collision (reassigned from solver upgrade)
**What:** Cloth now collides with ANY scene mesh via Unreal's Global Distance Field (GDF), in
addition to the analytic colliders. A minimal `FClothSceneViewExtension` snapshots the scene's
GDF parameters (`UE::FXRenderingUtils::GetGlobalDistanceFieldParameterData`) and the view's
uniform buffer each frame; `ClothCollisionDF.usf` samples `GetDistanceToNearestSurfaceGlobal` +
`GetDistanceFieldGradientGlobal` to push penetrating particles out along the surface gradient,
with the same tangential friction as M5. Toggle: `bUseDistanceFieldCollision`. Also added a
collider wireframe debug draw (`bDrawColliders`) so the analytic shapes are visible.

**Why:** "Collide with any mesh" — distance fields are how Niagara does GPU collision against
arbitrary geometry. Highest portfolio value of the project.

**Problems & solutions (this was the hardest milestone):**
- *Where to get the GDF:* it's renderer-owned. Used the public `FXRenderingUtils` accessor.
- *Hook timing:* first captured in `PreRenderView_RenderThread` → GDF not built yet → always null
  (`valid=0`). Moved capture to `PostRenderBasePassDeferred_RenderThread`, after the GDF exists.
- *Crash `expected uniform buffer at slot 1 (View)`:* `GlobalDistanceFieldShared.ush` transitively
  references `ResolvedView`, so the shader requires the engine `View` UB even though the GDF inputs
  come from our standalone `FGlobalDistanceFieldParameters2`. Fixed by snapshotting
  `FSceneView::ViewUniformBuffer` (public) in the SVE and binding it via `SHADER_PARAMETER_STRUCT_REF`.
- *Standalone vs view-UB GDF params:* the header's non-material branch (`DISTANCE_FIELD_IN_VIEW_UB`
  off) declares the GDF inputs as loose params matching `FGlobalDistanceFieldParameters2`, so a
  compute shader can sample the GDF without being a material. Sampling is in translated world
  space (world + cached `PreViewTranslation`).
- *GDF generation:* requires "Generate Mesh Distance Fields" + a consumer; forced via
  `r.DistanceFieldAO=1` / `r.AOGlobalDistanceField=1` in DefaultEngine.ini ([SystemSettings]).
- Added an on-screen `ClothSim GDF: valid=/clipmaps=` diagnostic that was essential to isolate the
  hook-timing vs generation issues.

**Performance:** one extra compute pass/substep when enabled; one field sample + gradient per
particle. GDF is coarse, so collision is approximate on small/thin meshes.

**Next:** M7 — colored Gauss-Seidel solver + bending constraints.

---

## 2026-06-15 — M7: Colored Gauss-Seidel + Bending
**What:** Added a second distance solver alongside the M2 Jacobi gather. On `BeginPlay` the
component now builds an **explicit constraint buffer** (`FGPUConstraint{IndexA, IndexB,
RestLength, StiffScale}`): structural (rest=`Spacing`), shear (rest=`Spacing·√2`), and optional
**bending** (2-away neighbour, rest=`2·Spacing`, softer via `BendStiffness`). The constraints are
**greedy graph-colored** on the CPU so no two constraints in a color share a particle, then sorted
into a color-contiguous buffer with per-color `[Start,Count)` ranges. The new
`ClothSolveGaussSeidel.usf` runs **one thread per constraint**, projecting BOTH endpoints in place;
the dispatch loop issues **one pass per color** so within a pass writes are disjoint (race-free)
and across passes each color sees the previous color's corrections (true Gauss-Seidel). New
`EClothSolverMode` (Jacobi / Gauss-Seidel) toggles between paths at runtime for A/B comparison.

**Why:** Gauss-Seidel propagates corrections within a single iteration (no Jacobi
under-relaxation), so the cloth reaches a given stiffness in far fewer iterations. The explicit
constraint model also generalizes past the regular-grid neighbour assumption and is the natural
home for bending constraints, which stop the cloth folding to a crease.

**Problems & solutions:**
- *Race-free parallel Gauss-Seidel:* the whole point of graph coloring. Greedy coloring assigns
  each edge the smallest color absent from both endpoints' used-color sets (`TSet` per particle).
  A 32×32 grid with all three constraint families colors into a modest number of colors; each is a
  contiguous slice dispatched independently. RDG serializes the per-color UAV read-modify-write, so
  color *k+1* sees color *k*'s output without explicit barriers.
- *In-place vs ping-pong:* GS solves in place on a single predicted buffer (no `PredictedB`);
  Jacobi keeps the two-buffer ping-pong. Both paths converge on `In` so the existing
  Collision → DF → Finalize tail is unchanged.
- *Runtime vs build-time params:* `SolverMode` and global `Stiffness` are live; bending topology
  and `BendStiffness` are baked into the constraint buffer at `BeginPlay` (like grid params), so
  toggling `bUseBending` takes effect on the next play.

**Performance:** GS issues `Iterations × NumColors` small dispatches/substep (vs Jacobi's
`Iterations`), but converges in fewer iterations for equivalent stiffness; each constraint touches
exactly 2 particles (vs the gather's up-to-8 reads). Negligible at 1024 particles; the
iteration-count win is the real lever. Proper `stat GPU` comparison is the M8 profiling pass.

**Next:** M8 — debug-viz polish + profiling (Jacobi vs Gauss-Seidel convergence/ms capture).

---

## 2026-06-15 — M8: Debug Viz Polish + Profiling
**What:** Added (1) **strain visualization** — per-particle membrane strain (avg deviation of the
structural-neighbour edge lengths from rest, signed) mapped to a color ramp (blue=compressed,
green=rest, red=stretched), written both to the mesh's per-vertex **color buffer** (new per-frame
upload path in `FClothMeshSceneProxy::UpdateVertices_RenderThread`) and to the debug points so it's
visible with or without a vertex-color material; (2) an **on-screen stats** readout (`bShowStats`):
solver mode, particle/constraint/color counts, and **solve dispatches/substep** (the headline
Jacobi-vs-GS cost difference); (3) profiling visibility via the existing per-pass `RDG_EVENT_NAME`
labels (`ProfileGPU` / RenderDoc / Insights show every `ClothSolveDistance` / `ClothSolveGaussSeidel`
pass). New component props: `bVisualizeStrain`, `StrainScale`, `bShowStats`.

**Why:** Make the simulation legible — strain coloring shows *where* the cloth is under tension
(great portfolio visual + a debugging aid), and the stats/profiling expose the M7 solver trade-off
quantitatively.

**Problems & solutions:**
- *Editor crash on Play (RHI breadcrumb assertion `LocalCurrentBreadcrumb == Sentinel`,
  RenderGraphBuilder.cpp:1761).* Root cause: I wrapped the dispatch in `RDG_GPU_STAT_SCOPE` +
  `RDG_EVENT_SCOPE` to get a `stat GPU` "ClothSim" line, but those push **RHI breadcrumbs**, and
  this plugin runs its own `FRDGBuilder` on the **immediate command list inside an enqueued render
  command** — outside the renderer's managed breadcrumb scope — so the breadcrumb stack was
  unbalanced at `Execute()`. **Fix:** removed both scope macros; kept the per-pass `RDG_EVENT_NAME`
  labels (crash-safe, still profiler-visible). Lesson: don't open RHI breadcrumb scopes on a
  standalone RDG builder driven from a render command; rely on per-pass event names instead.
- *Vertex color buffer update:* the proxy already created a `ColorVertexBuffer` (init from
  `FDynamicMeshVertex.Color`) but never updated it. Added a `LockBuffer`+`memcpy` of a tightly
  packed `FColor` array (same pattern as the position buffer), guarded on a vertex-count match.

**Performance:** Strain is CPU-side, O(particles) over the readback we already consume; one extra
small vertex-buffer upload/frame. Negligible at 1024 particles.

**Next:** M+ (stretch) — zero-copy GPU vertex write, or further solver work (XPBD compliance /
true dihedral bending). Core roadmap (M1–M8) is complete.

---

## 2026-06-15 — M9: Self-Collision (+ drop-test support: orientation, ground plane, two-sided normal fix)
**What:** Cloth-vs-itself collision on the GPU via a **spatial hash grid** (Teschner-style).
`ClothBuildGrid.usf` bins particles into a hash table (cell size = collision thickness) using
`InterlockedAdd` to append indices per bucket; `ClothSelfCollision.usf` has each particle scan the
27 neighbour cells and repel any **non-adjacent** particle closer than the thickness (Jacobi gather:
reads a snapshot, writes its own slot → race-free). It ping-pongs `PredictedA/B`, runs after the
distance solve and before external colliders, and loops `SelfCollisionIterations` times per substep
(rebuilding the grid each iteration) to clear deeper stacks. Thickness = `SelfCollisionScale·Spacing`.
Also added, to make self-collision testable and the demo complete:
- **Cloth Orientation** (`EClothOrientation`: vertical curtain / horizontal sheet) so cloth can be
  dropped flat instead of only hung.
- **Built-in ground plane** (`bGroundPlane` + `GroundHeight`) folded into `ClothCollision.usf`
  (infinite +Z plane + friction) so unpinned cloth rests on a floor without needing a giant
  sphere/capsule or the GDF.
- **Self-collision debug** (`bDebugSelfCollision`): CPU overlap check from the readback that draws
  red points on contacting particles and reports a contact count on screen.

**Why:** Self-collision is the last missing collision type (a folded/draped sheet previously passed
through itself). The orientation + ground plane make a clean, repeatable drop test possible.

**Problems & solutions:**
- *Atomics in the broadphase:* binning needs `InterlockedAdd` (the one place the project uses
  atomics). Confined to the grid build; the collision response stays race-free Jacobi gather.
  Buckets overflow past `MAX_PER_CELL` (16) drop extras — a documented approximation.
- *Cleared-counter buffer:* `AddClearUAVPass` + atomics want a **typed** `Buffer<uint>`, so
  `CellCounts` is typed (PF_R32_UINT) while `CellParticles` stays a structured index list.
- *Clipping still possible:* point-particle repulsion can't guarantee zero triangle interpenetration
  (that needs vertex-triangle / edge-edge CCD). Mitigated by raising the default thickness to ≈Spacing
  (so repulsion spheres overlap into a continuous barrier), stiffness to 1.0, and iterations to 2;
  more `Substeps` cut tunnelling. Honest limitation, documented as a future CCD milestone.
- ***Two-sided "dark face" bug (the tricky one):*** after adding the horizontal orientation, one face
  rendered black and never responded to the sun, in BOTH orientations, with a two-sided material.
  Root cause: smooth normals were `Cross(E1,E2)` (right-handed), but UE is **left-handed** and a
  two-sided material flips the vertex normal by **triangle winding** (`effectiveN = isFront ? N : -N`).
  Our normal disagreed with the winding, so the visible face was shaded with an inward normal → black
  regardless of light. Invisible with a one-sided material (only the front face is seen), which is why
  M3–M8 looked fine. Fixed by computing `Cross(E2,E1)` so the smooth normal agrees with UE's winding.
  Diagnosis path: a normal-line debug draw showed normals "pointing correctly" in world space, and
  Unlit looked fine + the face stayed black under any sun direction → isolated it to the two-sided
  VFACE/winding interaction rather than a normal-direction or material error. (Flipping the *winding*
  earlier did nothing because that flips winding and the derived normal together, preserving their
  relationship — the relationship was the bug.)

**Performance:** Self-collision adds, per substep, `SelfCollisionIterations × (clear + build + collide)`
small passes. Trivial at 1024 particles; the grid keeps it ~O(N·avgPerCell) rather than O(N²).

**Next:** M+ (stretch) — zero-copy GPU vertex write; or continuous (vertex-triangle/edge-edge)
self-collision for guaranteed clip-free contact; or XPBD compliance.
