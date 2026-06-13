#include "nabla/flow/poisson.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Geometric multigrid for the pressure Poisson (declared in poisson.hpp).
//
// The hierarchy is built by 2:1 agglomeration of the cell grid: a coarse cell
// is the parent of up to 2x2x2 fine cells, exactly the octree parent/child
// relation, so the same machinery carries over to AMR meshes (ADR-0007). The
// operator on every level is CgPoisson::applyA (matrix-free -Laplacian with the
// grid's Neumann/Dirichlet/periodic/solid closure), so operator and smoother
// can never drift. Smoother: damped Jacobi. Transfers: volume-weighted
// full-weighting restriction + piecewise-constant (conservative) prolongation.
// Coarsest level: near-exact CG. The cycle is wrapped in flexible CG (MG-PCG)
// for robustness with the immersed solid and the mixed BCs.

namespace nabla::flow {

struct MgPoisson::Level {
  UniformGrid grid;
  std::vector<double> diag;    // main diagonal of A (CgPoisson::buildDiagonal)
  std::vector<uint8_t> fluid;  // 1 = fluid cell
  std::size_t nFluid = 0;
  int fx = 1, fy = 1, fz = 1;  // coarsening factor from the next-finer level
};

struct MgPoisson::Hierarchy {
  std::vector<Level> levels;
  bool singular = false;        // pure-Neumann (no Dirichlet/outlet) -> null space
  std::uint64_t signature = 0;  // grid identity, for cache invalidation
};

namespace {

constexpr double kOmega = 0.8;             // damped-Jacobi relaxation factor
constexpr int kMaxLevels = 8;
constexpr std::size_t kCoarsestMax = 2048;  // stop coarsening at/under this size
constexpr int kMinCoarsen = 4;              // only halve an axis of length >= this

std::uint64_t hashGrid(const UniformGrid& g) {
  std::uint64_t h = 1469598103934665603ull;
  const auto mix = [&](std::uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix(static_cast<std::uint64_t>(g.nx));
  mix(static_cast<std::uint64_t>(g.ny));
  mix(static_cast<std::uint64_t>(g.nz));
  for (int f = 0; f < 6; ++f) {
    mix(static_cast<std::uint64_t>(g.face[f].type));
  }
  for (std::size_t i = 0; i < g.solid.size(); ++i) {
    if (g.solid[i]) {
      mix(i);
    }
  }
  return h;
}

void fillLevelMeta(MgPoisson::Level& L) {
  CgPoisson::buildDiagonal(L.grid, L.diag);
  const std::size_t n = L.grid.cellCount();
  L.fluid.assign(n, 0);
  L.nFluid = 0;
  for (int k = 0; k < L.grid.nz; ++k) {
    for (int j = 0; j < L.grid.ny; ++j) {
      for (int i = 0; i < L.grid.nx; ++i) {
        if (!L.grid.isSolid(i, j, k)) {
          L.fluid[L.grid.cidx(i, j, k)] = 1;
          ++L.nFluid;
        }
      }
    }
  }
}

// Remove the constant null-space mode (mean over fluid cells); solid cells -> 0.
void removeMean(const MgPoisson::Level& L, std::vector<double>& x) {
  if (L.nFluid == 0) {
    return;
  }
  double mean = 0.0;
  for (std::size_t c = 0; c < x.size(); ++c) {
    if (L.fluid[c]) {
      mean += x[c];
    } else {
      x[c] = 0.0;
    }
  }
  mean /= static_cast<double>(L.nFluid);
  for (std::size_t c = 0; c < x.size(); ++c) {
    if (L.fluid[c]) {
      x[c] -= mean;
    }
  }
}

// One or more damped-Jacobi sweeps: x <- x + omega D^{-1} (b - A x).
void smooth(const MgPoisson::Level& L, const std::vector<double>& b, std::vector<double>& x,
            int sweeps, bool singular) {
  std::vector<double> Ax(L.grid.cellCount());
  for (int s = 0; s < sweeps; ++s) {
    CgPoisson::applyA(L.grid, x, Ax);
    for (std::size_t c = 0; c < x.size(); ++c) {
      x[c] = L.fluid[c] ? x[c] + kOmega * (b[c] - Ax[c]) / L.diag[c] : 0.0;
    }
    if (singular) {
      removeMean(L, x);
    }
  }
}

void residual(const MgPoisson::Level& L, const std::vector<double>& b,
              const std::vector<double>& x, std::vector<double>& r) {
  CgPoisson::applyA(L.grid, x, r);  // r = A x
  for (std::size_t c = 0; c < r.size(); ++c) {
    r[c] = L.fluid[c] ? b[c] - r[c] : 0.0;
  }
}

// Full-weighting restriction of a fine residual onto the coarse grid: each
// coarse (parent) cell receives the volume-weighted average of its fluid
// children. Equal cell volumes => plain average.
void restrictResidual(const MgPoisson::Level& fine, const MgPoisson::Level& coarse,
                      const std::vector<double>& rf, std::vector<double>& rc, bool singular) {
  const std::size_t nc = coarse.grid.cellCount();
  rc.assign(nc, 0.0);
  std::vector<int> count(nc, 0);
  for (int k = 0; k < fine.grid.nz; ++k) {
    for (int j = 0; j < fine.grid.ny; ++j) {
      for (int i = 0; i < fine.grid.nx; ++i) {
        const std::size_t f = fine.grid.cidx(i, j, k);
        if (!fine.fluid[f]) {
          continue;
        }
        const std::size_t C =
            coarse.grid.cidx(i / coarse.fx, j / coarse.fy, k / coarse.fz);
        rc[C] += rf[f];
        ++count[C];
      }
    }
  }
  for (std::size_t C = 0; C < nc; ++C) {
    if (count[C] > 0) {
      rc[C] /= static_cast<double>(count[C]);
    }
  }
  if (singular) {
    removeMean(coarse, rc);
  }
}

// Piecewise-constant (conservative) prolongation: each fine child adds its
// parent coarse correction.  x_fine += P e_coarse.
void prolongAdd(const MgPoisson::Level& coarse, const MgPoisson::Level& fine,
                const std::vector<double>& ec, std::vector<double>& xf) {
  for (int k = 0; k < fine.grid.nz; ++k) {
    for (int j = 0; j < fine.grid.ny; ++j) {
      for (int i = 0; i < fine.grid.nx; ++i) {
        const std::size_t f = fine.grid.cidx(i, j, k);
        if (!fine.fluid[f]) {
          continue;
        }
        const std::size_t C =
            coarse.grid.cidx(i / coarse.fx, j / coarse.fy, k / coarse.fz);
        xf[f] += ec[C];
      }
    }
  }
}

void vcycle(MgPoisson::Hierarchy& H, std::size_t lvl, const std::vector<double>& b,
            std::vector<double>& x, int pre, int post) {
  MgPoisson::Level& L = H.levels[lvl];
  if (lvl + 1 >= H.levels.size()) {
    // Coarsest level: near-exact solve with CG (grid is tiny). CgPoisson
    // handles the null space for singular (pure-Neumann) systems itself.
    CgPoisson coarse(1e-6, 500);
    coarse.solve(L.grid, b, x);
    return;
  }
  smooth(L, b, x, pre, H.singular);
  std::vector<double> r;
  residual(L, b, x, r);
  std::vector<double> rc;
  restrictResidual(L, H.levels[lvl + 1], r, rc, H.singular);
  std::vector<double> ec(H.levels[lvl + 1].grid.cellCount(), 0.0);
  vcycle(H, lvl + 1, rc, ec, pre, post);
  prolongAdd(H.levels[lvl + 1], L, ec, x);
  smooth(L, b, x, post, H.singular);
}

double dot(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    s += a[i] * b[i];
  }
  return s;
}

}  // namespace

PoissonSolver::Report MgPoisson::solve(const UniformGrid& grid, const std::vector<double>& b,
                                       std::vector<double>& x) {
  // (Re)build the hierarchy only when the grid (dims / BCs / solid mask) changes
  // — a dynamic AMR step invalidates the cache, a fixed mesh builds it once.
  const std::uint64_t sig = hashGrid(grid);
  if (!cache_ || cache_->signature != sig) {
    auto H = std::make_shared<Hierarchy>();
    H->signature = sig;
    H->singular = !CgPoisson::hasDirichlet(grid);
    Level fine;
    fine.grid = grid;
    fillLevelMeta(fine);
    H->levels.push_back(std::move(fine));
    while (static_cast<int>(H->levels.size()) < kMaxLevels) {
      const UniformGrid& f = H->levels.back().grid;
      const int fx = (f.nx >= kMinCoarsen) ? 2 : 1;
      const int fy = (f.ny >= kMinCoarsen) ? 2 : 1;
      const int fz = (f.nz >= kMinCoarsen) ? 2 : 1;  // nz==1 stays 1
      if (fx == 1 && fy == 1 && fz == 1) {
        break;  // cannot coarsen further
      }
      Level c;
      c.fx = fx;
      c.fy = fy;
      c.fz = fz;
      UniformGrid& cg = c.grid;
      cg.nx = (f.nx + fx - 1) / fx;
      cg.ny = (f.ny + fy - 1) / fy;
      cg.nz = (f.nz + fz - 1) / fz;
      cg.dx = f.dx * static_cast<double>(f.nx) / static_cast<double>(cg.nx);
      cg.dy = f.dy * static_cast<double>(f.ny) / static_cast<double>(cg.ny);
      cg.dz = f.dz * static_cast<double>(f.nz) / static_cast<double>(cg.nz);
      cg.origin = f.origin;
      for (int fc = 0; fc < 6; ++fc) {
        cg.face[fc] = f.face[fc];
      }
      cg.allocate();
      // A coarse cell is fluid if ANY of its fine children is fluid (so the
      // fluid domain never disconnects under coarsening); solid otherwise.
      const Level& fineLvl = H->levels.back();
      std::vector<uint8_t> sawFluid(cg.cellCount(), 0);
      for (int k = 0; k < f.nz; ++k) {
        for (int j = 0; j < f.ny; ++j) {
          for (int i = 0; i < f.nx; ++i) {
            if (fineLvl.fluid[f.cidx(i, j, k)]) {
              sawFluid[cg.cidx(i / fx, j / fy, k / fz)] = 1;
            }
          }
        }
      }
      for (std::size_t cc = 0; cc < cg.solid.size(); ++cc) {
        cg.solid[cc] = sawFluid[cc] ? 0 : 1;
      }
      fillLevelMeta(c);
      H->levels.push_back(std::move(c));
      if (H->levels.back().grid.cellCount() <= kCoarsestMax) {
        break;
      }
    }
    cache_ = H;
  }

  Hierarchy& H = *cache_;
  const std::size_t N = grid.cellCount();
  x.assign(N, 0.0);

  std::vector<double> rhs = b;
  for (std::size_t c = 0; c < N; ++c) {
    if (!H.levels[0].fluid[c]) {
      rhs[c] = 0.0;
    }
  }
  if (H.singular) {
    removeMean(H.levels[0], rhs);
  }
  const double bnorm = std::sqrt(dot(rhs, rhs)) + 1e-300;

  const auto Minv = [&](const std::vector<double>& rr, std::vector<double>& zz) {
    zz.assign(N, 0.0);
    vcycle(H, 0, rr, zz, pre_, post_);
    if (H.singular) {
      removeMean(H.levels[0], zz);
    }
  };

  Report rep;
  std::vector<double> r = rhs;
  if (std::sqrt(dot(r, r)) <= tol_ * bnorm) {
    rep.iterations = 0;
    rep.residual = std::sqrt(dot(r, r)) / bnorm;
    rep.converged = true;
    return rep;
  }

  // Flexible CG (Polak-Ribiere) preconditioned by one multigrid V-cycle; the
  // flexible form tolerates the V-cycle's mild non-symmetry.
  std::vector<double> z, p, Ap, rnew, znew;
  Minv(r, z);
  p = z;
  double rz = dot(r, z);
  int it = 0;
  for (; it < maxCycles_; ++it) {
    CgPoisson::applyA(grid, p, Ap);
    const double pAp = dot(p, Ap);
    if (std::abs(pAp) < 1e-300) {
      break;
    }
    const double alpha = rz / pAp;
    rnew.assign(N, 0.0);
    for (std::size_t c = 0; c < N; ++c) {
      x[c] += alpha * p[c];
      rnew[c] = r[c] - alpha * Ap[c];
    }
    if (H.singular) {
      removeMean(H.levels[0], rnew);
    }
    if (std::sqrt(dot(rnew, rnew)) <= tol_ * bnorm) {
      ++it;
      r = rnew;
      rep.converged = true;
      break;
    }
    Minv(rnew, znew);
    double rnewz = 0.0, rz_cross = 0.0;
    for (std::size_t c = 0; c < N; ++c) {
      rnewz += znew[c] * rnew[c];
      rz_cross += znew[c] * r[c];
    }
    const double beta = (rnewz - rz_cross) / rz;  // flexible (PR) beta
    for (std::size_t c = 0; c < N; ++c) {
      p[c] = znew[c] + beta * p[c];
    }
    rz = rnewz;
    r = rnew;
  }

  if (H.singular) {
    removeMean(H.levels[0], x);
  }
  CgPoisson::applyA(grid, x, Ap);
  double rr = 0.0;
  for (std::size_t c = 0; c < N; ++c) {
    const double e = rhs[c] - Ap[c];
    rr += e * e;
  }
  rep.iterations = it;
  rep.residual = std::sqrt(rr) / bnorm;
  rep.converged = rep.converged || rep.residual <= tol_;
  return rep;
}

}  // namespace nabla::flow
