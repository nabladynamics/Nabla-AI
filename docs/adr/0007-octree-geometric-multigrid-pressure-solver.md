# 0007. Octree geometric multigrid for the pressure Poisson solve

- **Status:** Accepted
- **Date:** 2026-06-13
- **Deciders:** Founding engineering team
- **Supersedes:** the pressure-solver choice in
  [ADR-0003](0003-baseline-ns-solver.md) ("CG default, direct/multigrid
  pluggable"). The `PoissonSolver` interface and the matrix-free `−∇²` operator
  from ADR-0003 are unchanged; only the default backend changes.

## Context and Problem Statement

On the Re_h=500 wall-mounted cube the pressure projection dominated cost and
correctness:

- The default `CgPoisson` (Jacobi-preconditioned conjugate gradient) took
  **~222 iterations per solve** (≈660 per step over the three SSP-RK3
  projections) to reach a 1e-9 relative residual. Jacobi-CG on a
  Neumann-dominated 3-D Poisson scales like O(N^{1/3}) ≈ O(longest grid
  dimension) iterations, so it only gets worse as the mesh grows — and the
  whole point of Phase 1 is **dynamic AMR that adds cells**.
- ADR-0003 floated Eigen `SparseLU` as the "direct" alternative. A sparse direct
  factorization is a non-starter under dynamic AMR: **the matrix sparsity
  pattern changes every time the mesh adapts**, so the factorization must be
  recomputed from scratch on every adaptation — superlinear in N and dominant in
  3-D. Direct solves also do not parallelize or scale to the cell counts AMR is
  meant to reach.

We need a pressure solver whose cost is O(N), whose iteration count does **not**
grow with mesh size, and which is cheap to rebuild when the mesh changes.

## Decision

Replace the default pressure backend with **geometric multigrid, run as the
preconditioner for conjugate gradient (MG-PCG)** — `flow::MgPoisson`, behind the
unchanged `PoissonSolver` interface. `CgPoisson` is kept for tests and as a
dependency-free fallback.

The multigrid hierarchy is built by **2:1 agglomeration of the cell grid**: a
coarse cell is the parent of up to 2×2×2 fine cells. This is *exactly* the
octree parent/child relation — on the uniform Phase-0 grid the hierarchy is
plain structured coarsening, and on an adaptive octree mesh (Phase 1) the
**octree levels are the multigrid levels** with no new machinery. Components:

- **Operator:** the same matrix-free `−∇²` (`CgPoisson::applyA`) on every level,
  with the grid's Neumann (walls/inlet/fluid–solid) / Dirichlet (outlet) /
  periodic / solid-cell closure. Operator and smoother therefore can never
  drift, and the coarse operators are re-discretized (not assembled), so a mesh
  change just rebuilds light per-level metadata — O(N), no refactorization.
- **Smoother:** damped Jacobi (ω = 0.8), 2 pre- and 2 post-sweeps.
- **Restriction:** volume-weighted full-weighting (average of the fluid
  children) — conservative.
- **Prolongation:** piecewise-constant (conservative) injection to the children.
- **Coarsest level:** near-exact CG (the grid is ≤ ~2k cells).
- **Null space:** pure-Neumann systems (lid cavity) are singular; the constant
  mode is projected out of the RHS, every smoother sweep, and the result — the
  same compatible-subspace handling ADR-0003 specified, applied per level.
- **Outer iteration:** flexible (Polak–Ribière) CG, which tolerates the mild
  non-symmetry of the agglomeration V-cycle and is robust against the immersed
  solid and the mixed Neumann/Dirichlet boundary.

The hierarchy is cached and rebuilt only when the grid signature (dims / BCs /
solid mask) changes, so a fixed mesh builds it once and an AMR step invalidates
it automatically.

### Why not the alternatives

- **Sparse direct (Eigen SparseLU):** prohibitive under AMR (refactorize on
  every mesh change); also pulls in an MPL-2.0 dependency we kept optional.
- **Plain CG / better-preconditioned CG (IC0, etc.):** still mesh-size-dependent
  iteration counts; incomplete-Cholesky factors also need rebuilding on adapt.
- **Algebraic multigrid (AMG):** would work, but the setup phase (strength-of-
  connection, coarsening) is the expensive part and must rerun on every adapt;
  geometric agglomeration gets the hierarchy for free from the mesh itself.

## Consequences

- **Positive:**
  - Pressure solve drops from ~222 to **~14–15 cycles/solve** on the Re_h=500
    cube (≥1e-9 relative residual) — a ~15× reduction — and the cycle count is
    mesh-size-independent, so it stays flat as AMR adds cells.
  - O(N) per solve and rebuild-cheap on mesh change: the design is ready for the
    Phase-1 octree without a second solver.
  - No new third-party dependency (the in-house permissive operator is reused);
    the no-GPL core rule is unaffected.
- **Negative / trade-offs:**
  - Agglomeration with piecewise-constant transfers has a worse asymptotic
    convergence factor than full trilinear-interpolation geometric MG; wrapping
    in CG recovers the speed and robustness, at the cost of storing a few extra
    vectors. Trilinear prolongation is a possible future refinement.
  - The coarsest-level CG is a small serial bottleneck (negligible at current
    sizes; revisit with a redistributed coarse solve at scale).
- **Follow-ups:** drive the real octree levels directly in Phase 1 (the
  agglomeration machinery already matches); optional trilinear prolongation;
  true cut-cell (partial-volume) operator to replace the staircased immersed
  boundary — orthogonal to this solver choice.

## Note: a separate projection fix shipped alongside this change

While diagnosing the solver, the **rising continuity residual** reported on the
cube turned out **not** to be a solver problem: the pressure solve was
converging, but the trailing `applyVelocityBC` re-applied the convective-outflow
copy `u_outlet = u_interior` *after* the projection, overwriting the
divergence-free outlet face and re-injecting a growing divergence every stage.
The post-projection BC now leaves the projected outflow untouched
(`enforceOutflow=false`); the post-projection L2 divergence fell from ~1e-7
(rising to ~8e-7) to **~2e-11 and flat**, and Cd stopped drifting. This is a
projection/boundary-closure fix, recorded here for context but independent of
the multigrid decision.
