# 0002. Implement the adaptive octree from scratch (no wrapped AMR library)

- **Status:** Accepted
- **Date:** 2026-06-11
- **Deciders:** Founding engineering team

## Context and Problem Statement

The adaptive mesh is the most performance-critical data structure in the
solver. Every cell update, flux, and gradient touches it, and it must:

- be **cache-efficient now** — flat, contiguous, structure-of-arrays (SoA),
  with no per-cell pointer chasing;
- be **GPU-portable later** — the same SoA buffers should map onto device
  memory with minimal change;
- support a **linear octree** with Morton-encoded cell IDs, 10+ levels of
  refinement, 2:1 balance, refine/coarsen with conservative interpolation,
  cross-level neighbor/stencil queries, and solid masking;
- depend **only on permissively licensed code** (BSD / MIT / Apache / Boost /
  HDF5) — **no GPL** (see [ADR-0001](0001-local-first-client-server-architecture.md)
  and the core rule in `CLAUDE.md`).

The question: do we build the octree ourselves, or wrap an existing adaptive-mesh
library?

## Decision Drivers

- **License.** The no-GPL rule is hard. It eliminates the most popular cell-based
  octree libraries outright.
- **Memory-layout control.** Cache efficiency today and GPU portability tomorrow
  both depend on *us* owning the exact in-memory layout (SoA, flat arrays,
  index-based topology). A wrapped library dictates its own layout.
- **Scope.** A linear (pointer-free) octree keyed by Morton codes, with 2:1
  balance and injection/averaging transfer, is a well-understood and modest
  amount of code.
- **Dependency weight.** Heavyweight frameworks pull in build complexity, MPI,
  and their own data models that we would constantly translate to/from.

## Considered Options

| Option | License | Why it loses (or wins) |
| ------ | ------- | ---------------------- |
| **p4est** (forest of octrees) | **GPL-2.0+** | Disqualified by the no-GPL rule. |
| **libMesh** | LGPL-2.1 | Copyleft friction; heavy; general FEM data model, not a lean SoA octree. |
| **deal.II** | LGPL-2.1 | Same copyleft friction; very large; not our memory layout. |
| **AMReX** | BSD-3 (OK) | Permissive, but **block-structured** AMR (a different paradigm), large dependency, dictates its own layout/build (MPI-centric). |
| **From scratch** *(chosen)* | n/a | Full control of SoA layout; only permissive deps; scope is modest and now implemented + tested. |

## Decision

**We implement the octree from scratch.** Two drivers are decisive:

1. **License.** The best-fit cell-based octree, p4est, is GPL — forbidden in the
   core. The remaining established libraries are LGPL (friction we do not want at
   the heart of proprietary IP) or block-structured (AMReX), which is not the
   cell-based octree paradigm we want.
2. **Layout ownership.** The entire point of this data structure is cache
   efficiency and GPU portability, both of which require owning the memory
   layout. A wrapped library would force us to mirror or translate its
   structures, defeating the purpose.

### What we built (`core/.../mesh`, `core/.../io`)

- **Linear octree, leaves only.** Each leaf's ID is its Morton-encoded anchor
  (lower corner on the finest `2^maxLevel` grid), packed into a `uint64_t`
  (21 bits/axis → up to 21 levels, well past the 10+ target). A leaf's anchor
  Morton is unique among leaves, so a single open hash map gives O(1) lookup
  with no tree pointers. Parent/child/neighbor relations are pure bit
  arithmetic.
- **SoA storage.** Parallel arrays: `morton`, `level`, `mask`, a registry of
  named `double` fields (`u, v, w, p` always present, plus extensible scalars
  like `tke`, `dissipation`, `refine_indicator`), and a registry of named
  `uint8` labels (e.g., `physics_model`). Flat and contiguous — a natural fit
  for device buffers.
- **2:1 balance** enforced automatically: `refine` first refines any coarser
  face neighbor; `coarsen` refuses (preserving balance) if a merge would leave a
  neighbor two levels finer.
- **Conservative transfer:** refine uses piecewise-constant injection (the
  parent's integral is preserved exactly); coarsen uses volume-weighted
  averaging.
- **Cross-level neighbors and gradients**, solid masking (`Fluid / Cut /
  InsideSolid`), `.vtu` output (hand-written VTK XML — no VTK dependency), and
  HDF5 checkpoints.

### Dependencies introduced

- **HDF5** (checkpoints) — permissive BSD-style license; optional at build time.
- **VTK `.vtu`** output — **no** dependency; the XML is written by hand.
- **Catch2** (tests only, BSL-1.0).

All permissive. No GPL.

## Consequences

- **Positive**
  - Memory layout is ours: SoA, flat, pointer-free — the basis for cache
    efficiency and a later GPU port.
  - No GPL/LGPL anywhere in the core; the mesh stays freely re-licensable IP.
  - Small, dependency-light, and fully unit-tested (Morton round-trip, 2:1
    balance under random storms, integral conservation under refine/coarsen,
    checkpoint bit-exactness).

- **Negative / trade-offs**
  - We own and must maintain balance/neighbor/transfer logic that a library
    would have provided. Mitigated by the test suite and the modest scope.
  - Phase-0 uses a **single** octree with anisotropic physical cell sizes for a
    rectangular domain (cell aspect ratio = domain aspect ratio). Cubic cells
    would need a forest-of-octrees (a brick of roots) — a known, self-contained
    upgrade.
  - Distributed (MPI) AMR is out of scope for Phase 0.

- **Follow-ups**
  - Forest-of-octrees for cubic cells on non-cubic domains.
  - Higher-order conservative prolongation (slope-limited) when the solver needs
    it; the current injection is the conservative baseline.
  - If distributed AMR becomes necessary, re-evaluate a permissive option
    (e.g., AMReX, BSD-3) behind the same SoA-facing interface — *not* a GPL one.
