#include "nabla/flow/poisson.hpp"

#include <cmath>
#include <cstddef>

namespace nabla::flow {
namespace {

// Per-axis neighbor resolution for the pressure operator.
// Returns: kind 0 = no coupling (Neumann wall/inlet, or fluid-solid face),
//          kind 1 = coupling to interior fluid cell `n` with coeff 1/h^2,
//          kind 2 = Dirichlet x=0 face (outlet), self-coeff 2/h^2.
struct Face {
  int kind;
  std::size_t n;
};

Face resolveFace(const UniformGrid& g, int i, int j, int k, int axis, int side) {
  int ni = i, nj = j, nk = k;
  if (axis == 0) {
    ni += side;
  } else if (axis == 1) {
    nj += side;
  } else {
    nk += side;
  }
  const int n[3] = {g.nx, g.ny, g.nz};
  const int an = n[axis];
  int* ac = (axis == 0) ? &ni : (axis == 1 ? &nj : &nk);

  if (*ac >= 0 && *ac < an) {
    // interior neighbor
    if (g.isSolid(ni, nj, nk)) {
      return {0, 0};
    }
    return {1, g.cidx(ni, nj, nk)};
  }
  // crossing the domain boundary on this axis
  const int faceId = 2 * axis + (side > 0 ? 1 : 0);
  const BoundaryType bt = g.face[faceId].type;
  if (bt == BoundaryType::Periodic) {
    *ac = (*ac + an) % an;
    if (g.isSolid(ni, nj, nk)) {
      return {0, 0};
    }
    return {1, g.cidx(ni, nj, nk)};
  }
  if (bt == BoundaryType::Outlet) {
    return {2, 0};
  }
  return {0, 0};  // Wall / Inlet -> Neumann
}

double diagOf(const UniformGrid& g, int i, int j, int k) {
  const double hx2 = 1.0 / (g.dx * g.dx);
  const double hy2 = 1.0 / (g.dy * g.dy);
  const double hz2 = 1.0 / (g.dz * g.dz);
  const double h2[3] = {hx2, hy2, hz2};
  double d = 0.0;
  for (int axis = 0; axis < 3; ++axis) {
    if (axis == 2 && g.nz == 1) {
      continue;  // 2D: no z coupling
    }
    for (int side = -1; side <= 1; side += 2) {
      const Face f = resolveFace(g, i, j, k, axis, side);
      if (f.kind == 1) {
        d += h2[axis];
      } else if (f.kind == 2) {
        d += 2.0 * h2[axis];
      }
    }
  }
  return d;
}

}  // namespace

void CgPoisson::buildDiagonal(const UniformGrid& g, std::vector<double>& diag) {
  diag.assign(g.cellCount(), 1.0);
  for (int k = 0; k < g.nz; ++k) {
    for (int j = 0; j < g.ny; ++j) {
      for (int i = 0; i < g.nx; ++i) {
        if (g.isSolid(i, j, k)) {
          continue;  // identity row -> diag 1
        }
        const double d = diagOf(g, i, j, k);
        diag[g.cidx(i, j, k)] = (d > 0.0) ? d : 1.0;
      }
    }
  }
}

bool CgPoisson::hasDirichlet(const UniformGrid& g) {
  for (const FaceBC& f : g.face) {
    if (f.type == BoundaryType::Outlet) {
      return true;
    }
  }
  return false;
}

void CgPoisson::applyA(const UniformGrid& g, const std::vector<double>& x,
                       std::vector<double>& out) {
  const double h2[3] = {1.0 / (g.dx * g.dx), 1.0 / (g.dy * g.dy), 1.0 / (g.dz * g.dz)};
  out.assign(x.size(), 0.0);
  for (int k = 0; k < g.nz; ++k) {
    for (int j = 0; j < g.ny; ++j) {
      for (int i = 0; i < g.nx; ++i) {
        const std::size_t c = g.cidx(i, j, k);
        if (g.isSolid(i, j, k)) {
          out[c] = x[c];  // identity row -> stays 0
          continue;
        }
        double diag = 0.0;
        double off = 0.0;
        for (int axis = 0; axis < 3; ++axis) {
          if (axis == 2 && g.nz == 1) {
            continue;
          }
          for (int side = -1; side <= 1; side += 2) {
            const Face f = resolveFace(g, i, j, k, axis, side);
            if (f.kind == 1) {
              diag += h2[axis];
              off += h2[axis] * x[f.n];
            } else if (f.kind == 2) {
              diag += 2.0 * h2[axis];
            }
          }
        }
        out[c] = diag * x[c] - off;
      }
    }
  }
}

PoissonSolver::Report CgPoisson::solve(const UniformGrid& g, const std::vector<double>& b,
                                       std::vector<double>& x) {
  const std::size_t N = g.cellCount();
  x.assign(N, 0.0);

  // Diagonal (Jacobi preconditioner). Solid cells use 1 (identity rows).
  std::vector<double> diag(N, 1.0);
  std::vector<uint8_t> fluid(N, 0);
  std::size_t nFluid = 0;
  for (int k = 0; k < g.nz; ++k) {
    for (int j = 0; j < g.ny; ++j) {
      for (int i = 0; i < g.nx; ++i) {
        const std::size_t c = g.cidx(i, j, k);
        if (g.isSolid(i, j, k)) {
          continue;
        }
        fluid[c] = 1;
        ++nFluid;
        const double d = diagOf(g, i, j, k);
        diag[c] = (d > 0.0) ? d : 1.0;
      }
    }
  }

  // Working RHS; project onto the compatible subspace for pure-Neumann systems.
  std::vector<double> rhs = b;
  const bool singular = !hasDirichlet(g);
  if (singular && nFluid > 0) {
    double mean = 0.0;
    for (std::size_t c = 0; c < N; ++c) {
      if (fluid[c]) {
        mean += rhs[c];
      }
    }
    mean /= static_cast<double>(nFluid);
    for (std::size_t c = 0; c < N; ++c) {
      if (fluid[c]) {
        rhs[c] -= mean;
      } else {
        rhs[c] = 0.0;
      }
    }
  }

  const auto dot = [&](const std::vector<double>& a, const std::vector<double>& c) {
    double s = 0.0;
    for (std::size_t m = 0; m < N; ++m) {
      s += a[m] * c[m];
    }
    return s;
  };

  std::vector<double> r = rhs;  // x = 0 -> r = b
  std::vector<double> z(N), p(N), Ap(N);
  for (std::size_t m = 0; m < N; ++m) {
    z[m] = r[m] / diag[m];
  }
  p = z;
  double rz = dot(r, z);
  const double bnorm = std::sqrt(dot(rhs, rhs)) + 1e-300;

  Report rep;
  int it = 0;
  for (; it < maxIters_; ++it) {
    applyA(g, p, Ap);
    const double pAp = dot(p, Ap);
    if (std::abs(pAp) < 1e-300) {
      break;
    }
    const double alpha = rz / pAp;
    for (std::size_t m = 0; m < N; ++m) {
      x[m] += alpha * p[m];
      r[m] -= alpha * Ap[m];
    }
    const double rnorm = std::sqrt(dot(r, r));
    if (rnorm <= tol_ * bnorm) {
      ++it;
      rep.converged = true;
      break;
    }
    for (std::size_t m = 0; m < N; ++m) {
      z[m] = r[m] / diag[m];
    }
    const double rzNew = dot(r, z);
    const double beta = rzNew / rz;
    for (std::size_t m = 0; m < N; ++m) {
      p[m] = z[m] + beta * p[m];
    }
    rz = rzNew;
  }

  if (singular && nFluid > 0) {
    double mean = 0.0;
    for (std::size_t c = 0; c < N; ++c) {
      if (fluid[c]) {
        mean += x[c];
      }
    }
    mean /= static_cast<double>(nFluid);
    for (std::size_t c = 0; c < N; ++c) {
      if (fluid[c]) {
        x[c] -= mean;
      }
    }
  }

  applyA(g, x, Ap);
  double rr = 0.0;
  for (std::size_t m = 0; m < N; ++m) {
    const double e = rhs[m] - Ap[m];
    rr += e * e;
  }
  rep.iterations = it;
  rep.residual = std::sqrt(rr) / bnorm;
  rep.converged = rep.converged || rep.residual <= 1e-6;
  return rep;
}

}  // namespace nabla::flow
