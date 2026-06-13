# 0008. Octree-native flow solve (collocated finite volume)

- **Status:** Accepted (implementation in progress — increment 1 landed)
- **Date:** 2026-06-13
- **Deciders:** Founding engineering team
- **Relates to:** [ADR-0002](0002-octree-from-scratch.md) (octree mesh),
  [ADR-0003](0003-baseline-ns-solver.md) (uniform MAC baseline),
  [ADR-0004](0004-adaptive-layer.md) (adaptive layer),
  [ADR-0007](0007-octree-geometric-multigrid-pressure-solver.md) (multigrid).

## Context and Problem Statement

The adaptive layer (ADR-0004) refines an **octree that is decoupled from the
flow solve**. Concretely, in `runAdaptiveCube` the flow is integrated by
`BaselineBackend` on a *fixed uniform MAC grid*, and the octree is only a
**shadow mesh**: `sampleFlow(tree, backend)` interpolates the uniform field
*onto* the octree for the sensors / physics labels / fidelity map, but nothing
ever feeds the refined octree *back* into the momentum/pressure solve. The
consequence on the Re_h=500 cube: refining the octree changes `tree.cellCount()`
and the fidelity map, but the **flow is always solved at the uniform base
resolution** — so the field stays blocky and the upstream floor separation is
stuck at ~0.37h instead of the ~1–2h the wall-mounted-cube literature expects.

For the adaptive mesh to actually improve the solution (the whole product
thesis), **the Navier–Stokes solve must run on the adaptive octree itself.**

## Decision

Add an **octree-native, cell-centered collocated finite-volume incompressible
NS solver** that integrates the flow on the adaptive octree leaves. The uniform
MAC solver (ADR-0003) is **kept unchanged** — it remains the validated reference
and the "uniform-fine baseline" for the speedup story; the new solver is added
alongside, so the validated solver can never regress.

Why collocated FV rather than porting staggered MAC to the octree:

- The octree stores `u,v,w,p` at **cell centers** already, and exposes
  `faceNeighbors` (1 coarse/equal or up to 4 fine across a 2:1 jump) and a
  level-jump-aware `gradient`. Cell-centered FV maps directly onto this.
- Staggered MAC on an adaptive octree requires defining face velocities at
  hanging faces and is notoriously fiddly; collocated FV with **Rhie–Chow**
  momentum interpolation is the standard, robust choice for unstructured/AMR
  incompressible solvers and avoids pressure–velocity checkerboarding.
- The pressure projection reuses the **agglomeration multigrid** from ADR-0007:
  octree parent/child *is* the multigrid hierarchy.

### Discretization

- **Operators (finite volume, conservative).** For a cell `P` with volume `V_P`,
  every face flux is `area_f · (φ_N − φ_P) / d_PN`. At a 2:1 interface a coarse
  cell sums the fluxes of its (up to 4) fine sub-faces and each fine cell sees
  the one coarse neighbour; both use the fine face area and the true
  centre-to-centre distance, so the flux a fine cell sends equals (with opposite
  sign) the flux the coarse cell receives — **discretely conservative across
  level jumps** (the property whose absence ADR-0007's note and R2 flagged as
  the source of mass drift / blockiness).
- **Pressure–velocity coupling:** Rhie–Chow face interpolation so the projection
  Poisson operator is the compact `div·grad` and the velocity field is made
  discretely divergence-free including across level interfaces.
- **Pressure solve:** octree geometric multigrid / MG-PCG (ADR-0007 extended to
  the octree levels).
- **Convection:** WENO-5 away from interfaces; at a 2:1 jump the wide stencil is
  filled with conservative interpolated ghost values and the order necessarily
  reduces *locally* at the hanging face (documented where and why), **not**
  across the whole near-body region — the over-dissipation R2 observed.
- **Time integration:** the SSP-RK3 projection scheme of ADR-0003, unchanged.

## Increment plan (each step re-validates; the validated MAC solver is untouched)

1. **Octree FV operators + Poisson + projection** *(this increment).* Divergence,
   gradient, Laplacian with conservative 2:1 flux matching; matrix-free Poisson
   (flexible-CG, the ADR-0007 outer iteration) on the octree operator;
   divergence-free projection. Manufactured-solution unit tests on a 2:1-refined
   octree: operator consistency and projection driving `div u → 0` across the
   interface.
2. **Momentum on the octree** (convection + diffusion, Rhie–Chow faces) →
   validate **octree Poiseuille** against the analytic parabola ≤2% L2.
3. **WENO-5 across 2:1 jumps** with conservative ghost interpolation; document
   the local order reduction at hanging faces.
4. **Couple to the adaptive loop**: the AMR controller refines the octree the
   solver runs on; calibrate flow-scale indicators; refinement budget (max
   cells/level, priority ranking); adaptive ON by default for the cube + explicit
   in config/UI; audit each refine/coarsen with its triggering indicator. Then
   the cube acceptance: separation → 1–2h, grid-convergence within the 5–10%
   band, and adaptive-matches-uniform-fine at lower cells/wall-time.

## Consequences

- **Positive:** the adaptive mesh finally improves the solution; the multigrid
  (ADR-0007) and octree (ADR-0002) investments pay off; the uniform MAC solver
  stays as an untouched, validated reference and baseline.
- **Negative / trade-offs:** a second flow discretization to maintain; collocated
  FV needs Rhie–Chow care to avoid checkerboarding; full delivery spans several
  increments. Mitigated by re-validating analytic cases at every increment and
  never modifying the validated MAC solver.
- **Follow-ups:** true cut-cell (partial-volume) solid treatment to replace the
  staircase (also improves the momentum-residual floor noted in ADR-0007);
  forest-of-octrees for cubic cells on non-cubic domains (ADR-0002).
