# nabla core

Headless C++20 CFD solver core. No UI dependencies; permissively licensed
dependencies only (BSD / MIT / Apache / Boost) — **never GPL**.

## Targets

| Target            | Kind          | Description                                       |
| ----------------- | ------------- | ------------------------------------------------- |
| `nabla_core`      | static lib    | Fields, solvers, octree AMR, and IPC/IO helpers.  |
| `nabla_solve`     | executable    | CLI front-end; the binary the backend calls.      |
| `nabla_mesh_demo` | executable    | Builds an adaptive octree, writes `.vtu` + HDF5.  |
| `nabla_tests`     | executable    | Catch2 unit tests (test-only dependency).         |

## Modules

- `mesh/` — linear octree adaptive mesh: Morton-encoded cell IDs, SoA field
  storage, 2:1 balance, refine/coarsen, cross-level neighbors and gradients,
  solid masking. Design rationale in
  [ADR-0002](../docs/adr/0002-octree-from-scratch.md).
- `geometry/` — Stage-1 geometry capture: robust STL reader (binary + ASCII)
  with welding / degenerate removal / watertightness reporting; BVH-based
  classification of octree cells into `FLUID` / `CUT` / `INSIDE_SOLID` (CUT cells
  get a no-slip wall flag); sharp-edge / corner / curvature feature detection;
  geometry-driven refinement (extra at sharp edges); and a JSON geometry report.
- `flow/` — baseline incompressible Navier–Stokes solver on a uniform mesh:
  staggered MAC, SSP-RK3 fractional-step projection, WENO-5 / 2nd-order-central
  convection, central diffusion, and a pluggable pressure-Poisson interface
  (`PoissonSolver`) with a Jacobi-preconditioned CG default. No-slip / inlet /
  convective-outlet / periodic BCs, CFL-adaptive Δt, a per-step convergence gate,
  `diagnostics.jsonl`, `.vtu` snapshots and checkpoint/restart. Validated against
  Ghia (lid cavity) and analytic Poiseuille to ≤2% L2. See ADR-0003.
- `adaptive/` — physics-aware adaptive layer on the octree (ADR-0004): per-cell
  sensors (|ω|, |S|, Q, |∇p|, |∇u|, y+, τ_wall, truncation estimate, residual
  stagnation), an AMR controller with the spec's static-box minimum-resolution
  floors, a swappable reduced-model classifier whose proposals are only
  *accepted* by NS-consistency checks (reject ⇒ promote to FULL_NS + reason),
  unconditional hard guards, a step-acceptance controller, an efficiency metric,
  and a first-class **audit trail** (`audit.jsonl` + summary). Flow is advanced
  behind a `SolverBackend` seam (baseline-backed today; octree-native next).
- `io/` — `.vtu` (VTK XML, hand-written, opens in ParaView) and HDF5
  checkpoints (bit-exact full-state round-trip; optional via `NABLA_WITH_HDF5`).

```bash
# Build everything and run the AMR demo:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/nabla_mesh_demo out.vtu out.h5     # open out.vtu in ParaView

# Stage 1: ingest an STL into a meshed domain + geometry report:
./build/nabla_solve ingest examples/cube.stl --case wall-mounted-cube
#   -> cube.vtu (open in ParaView) + cube.geometry.json

# Baseline NS solve (wall-mounted cube at Re_h=500), restartable:
./build/nabla_solve run examples/cube_case.json
./build/nabla_solve run examples/cube_case.json --restart run_cube/checkpoint_20.ckpt
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/nabla_solve --version
ctest --test-dir build --output-on-failure
```

Disable tests (skips the Catch2 fetch) with `-DNABLA_BUILD_TESTS=OFF`.

## File-based IPC contract

`nabla_solve` reads a `key = value` spec file and writes a `key = value`
result file. This is the only coupling between the core and the backend — see
[`../CLAUDE.md`](../CLAUDE.md) and [ADR-0001](../docs/adr/0001-local-first-client-server-architecture.md).

```bash
printf 'nx = 64\nny = 64\nsteps = 200\n' > /tmp/case.spec
./build/nabla_solve --input /tmp/case.spec --output /tmp/case.result --field /tmp/case.field
```
