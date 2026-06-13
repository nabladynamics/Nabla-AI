# 0003. Baseline incompressible Navier–Stokes solver

- **Status:** Accepted (pressure-solver choice superseded by
  [ADR-0007](0007-octree-geometric-multigrid-pressure-solver.md))
- **Date:** 2026-06-11
- **Deciders:** Founding engineering team

> The default pressure backend is now geometric multigrid (MG-PCG), not
> Jacobi-CG; see [ADR-0007](0007-octree-geometric-multigrid-pressure-solver.md).
> The `PoissonSolver` interface and the matrix-free `−∇²` operator described
> below are unchanged.

## Context and Problem Statement

We need a **trustworthy baseline** incompressible Navier–Stokes solver: full
physics on every fluid cell, a fixed mesh, no reduced-physics models and no AMR
during the solve. The adaptive layer (next milestone) must be validated against
this baseline, so correctness and quantitative validation matter more than
performance or generality here.

The technical spec prescribes: RK3 fractional-step projection; WENO-5 convection;
2nd-order central diffusion; a sparse-direct pressure Poisson solve with a clean
seam for a future multigrid; no-slip / inlet / convective-outlet / periodic BCs;
CFL-adaptive Δt; a per-step convergence gate; a diagnostics stream; and two
analytic validations (lid-driven cavity vs Ghia, Poiseuille).

## Decisions

### Mesh: uniform Cartesian (a uniformly-refined octree)
"Fixed mesh, no AMR during the solve" + WENO-5 ⇒ a **uniform** mesh. WENO-5 across
octree hanging nodes is research-grade and is exactly what the *adaptive* prompt
owns. A uniformly-refined octree is a uniform Cartesian grid, so the baseline
runs on `flow::UniformGrid`; its kernels are written cell-/face-local so the AMR
layer can generalize them. `nz == 1` + periodic-z gives a clean 2D problem for
the validations.

### Discretization: staggered MAC
The validations must pass to ≤2%, so robustness is paramount. A **staggered MAC**
arrangement (u,v,w on faces, p at centers) gives a compact, exact discrete
divergence/gradient and a checkerboard-free projection — the standard, reliable
choice. (A collocated Rhie–Chow scheme would match the octree's cell-centered
storage but is more delicate; the AMR layer will revisit this. The baseline
favors trustworthiness.)

- **Time:** SSP-RK3, with a pressure projection after each stage (fractional
  step). Δt from a combined convective+viscous CFL limit (target CFL, default
  0.8/0.7).
- **Convection:** WENO-5 (Jiang–Shu) upwind reconstruction, *and* a 2nd-order
  central option. The smooth validation flows are scheme-insensitive and use
  central; the cube (Re_h=500) uses WENO-5 for robustness. Both are implemented.
- **Diffusion:** 2nd-order central finite volume.

### Pressure Poisson: interface + CG default, direct/multigrid pluggable
`flow::PoissonSolver` is an abstract interface (`solve(A x = b)` for
`A = −∇²` with the grid's BCs). The default `CgPoisson` is a Jacobi-preconditioned,
**matrix-free** conjugate gradient — fully permissive, zero external deps, and it
drives the validations to pass. Pure-Neumann (cavity) systems are singular; the
RHS is projected onto the compatible subspace and the constant null-space mode is
removed. A sparse-direct or geometric-multigrid backend can replace `CgPoisson`
behind the same interface without touching the NS core.

#### License note (req 4: "verify license")
The spec suggests Eigen's `SparseLU` as the direct option. **Eigen is MPL-2.0** —
a *weak, file-level copyleft*, **not GPL**: it can be linked into proprietary code,
and (unmodified) only requires offering Eigen's own source to recipients. That is
compatible with this proprietary, server-side core (the solver never ships to
clients; see ADR-0001). We nonetheless make Eigen **optional** and keep the
permissive in-house CG as the default, so CI and the validations need no MPL
dependency. We avoid Eigen's *external* backends (UmfPack/Cholmod/SuperLU
bindings) since some are LGPL/GPL.

### Boundary conditions
No-slip walls (incl. a moving lid and immersed staircase solids), inlet
(uniform + parabolic), convective/zero-gradient outlet, and periodic axes — all
applied through BC-aware staggered accessors and the Poisson operator's face
logic.

### Convergence gate & diagnostics (reqs 7–8)
Every step emits momentum residual, continuity residual (‖∇·u‖) and
mass-conservation error; a step is *accepted* when continuity and mass error are
below tolerance. One JSON line per step is appended to
`<run_dir>/diagnostics.jsonl` (step, t, dt, CFL, residuals, mass error, Cd, Cl,
cell count, Poisson iters). `.vtu` snapshots every N steps; a bit-exact binary
checkpoint every M steps with exact restart (`--restart`). HDF5 is the
production checkpoint format and the infra exists (`io/checkpoint`, ADR-0002); the
flow checkpoint uses a compact, dependency-free binary for portable exact restart.

## Consequences

- **Positive:** a correct, validated baseline (cavity vs Ghia and Poiseuille vs
  analytic, both ≤2% L2); a clean Poisson seam for multigrid; no GPL, no required
  external deps; full diagnostics + restart.
- **Trade-offs:** uniform mesh only (AMR is the next layer); staggered MAC differs
  from the octree's collocated storage (bridged by building the grid from a
  uniform octree and writing cell-centered `.vtu`); immersed solids are
  staircased at uniform resolution; non-incremental projection (steady-state
  accurate, adequate for these laminar cases).
- **Follow-ups:** Eigen/AMG Poisson backend; cut-cell (non-staircase) solids;
  generalize the kernels onto the adaptive octree.
