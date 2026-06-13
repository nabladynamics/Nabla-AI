#include "nabla/flow/octree_poisson.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nabla::flow {

using mesh::CellMask;
using mesh::Octree;
using mesh::Vec3;

namespace {

double axisOf(const Vec3& v, int axis) {
  return (axis == 0) ? v.x : (axis == 1 ? v.y : v.z);
}

bool isFluid(const Octree& t, std::size_t i) { return t.mask(i) == CellMask::Fluid; }

// Face coefficient area/dist between cell i and a face-neighbour j on `axis`:
// the finer cell's transverse face area divided by the true centre distance.
double faceCoeff(const Octree& t, std::size_t i, std::size_t j, int axis) {
  const int t1 = (axis + 1) % 3;
  const int t2 = (axis + 2) % 3;
  const std::size_t finer = (t.level(j) > t.level(i)) ? j : i;
  const Vec3 fs = t.cellSize(finer);
  const double area = axisOf(fs, t1) * axisOf(fs, t2);
  const double dist = std::abs(axisOf(t.cellCenter(i), axis) - axisOf(t.cellCenter(j), axis));
  return (dist > 0.0) ? area / dist : 0.0;
}

void removeMean(const std::vector<uint8_t>& fluid, std::size_t nFluid, std::vector<double>& x) {
  if (nFluid == 0) {
    return;
  }
  double mean = 0.0;
  for (std::size_t c = 0; c < x.size(); ++c) {
    if (fluid[c]) {
      mean += x[c];
    } else {
      x[c] = 0.0;
    }
  }
  mean /= static_cast<double>(nFluid);
  for (std::size_t c = 0; c < x.size(); ++c) {
    if (fluid[c]) {
      x[c] -= mean;
    }
  }
}

double dot(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    s += a[i] * b[i];
  }
  return s;
}

}  // namespace

void OctreePoisson::applyA(const Octree& tree, const std::vector<double>& x,
                           std::vector<double>& out) {
  const std::size_t n = tree.cellCount();
  out.assign(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    if (!isFluid(tree, i)) {
      out[i] = x[i];  // identity row for solid/cut cells
      continue;
    }
    double acc = 0.0;
    for (int dir = 0; dir < 6; ++dir) {
      const int axis = dir / 2;
      for (std::size_t j : tree.faceNeighbors(i, dir)) {
        if (!isFluid(tree, j)) {
          continue;  // homogeneous Neumann at a solid/cut face
        }
        acc += faceCoeff(tree, i, j, axis) * (x[i] - x[j]);
      }
    }
    out[i] = acc;
  }
}

void OctreePoisson::buildDiagonal(const Octree& tree, std::vector<double>& diag) {
  const std::size_t n = tree.cellCount();
  diag.assign(n, 1.0);
  for (std::size_t i = 0; i < n; ++i) {
    if (!isFluid(tree, i)) {
      continue;
    }
    double d = 0.0;
    for (int dir = 0; dir < 6; ++dir) {
      const int axis = dir / 2;
      for (std::size_t j : tree.faceNeighbors(i, dir)) {
        if (isFluid(tree, j)) {
          d += faceCoeff(tree, i, j, axis);
        }
      }
    }
    diag[i] = (d > 0.0) ? d : 1.0;
  }
}

OctreePoisson::Report OctreePoisson::solve(const Octree& tree, const std::vector<double>& b,
                                           std::vector<double>& x) {
  const std::size_t n = tree.cellCount();
  x.assign(n, 0.0);

  std::vector<uint8_t> fluid(n, 0);
  std::size_t nFluid = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (isFluid(tree, i)) {
      fluid[i] = 1;
      ++nFluid;
    }
  }
  // The octree domain is a closed box with no Dirichlet faces yet, so the system
  // is pure-Neumann (singular); the constant mode is removed. (Dirichlet outlet
  // support arrives with the cube coupling, ADR-0008 increment 4.)
  const bool singular = true;

  std::vector<double> diag;
  buildDiagonal(tree, diag);

  std::vector<double> rhs = b;
  for (std::size_t c = 0; c < n; ++c) {
    if (!fluid[c]) {
      rhs[c] = 0.0;
    }
  }
  if (singular) {
    removeMean(fluid, nFluid, rhs);
  }
  const double bnorm = std::sqrt(dot(rhs, rhs)) + 1e-300;

  std::vector<double> r = rhs, z(n, 0.0), p(n, 0.0), Ap(n, 0.0);
  for (std::size_t c = 0; c < n; ++c) {
    z[c] = fluid[c] ? r[c] / diag[c] : 0.0;
  }
  if (singular) {
    removeMean(fluid, nFluid, z);
  }
  p = z;
  double rz = dot(r, z);

  Report rep;
  int it = 0;
  for (; it < maxIters_; ++it) {
    applyA(tree, p, Ap);
    const double pAp = dot(p, Ap);
    if (std::abs(pAp) < 1e-300) {
      break;
    }
    const double alpha = rz / pAp;
    for (std::size_t c = 0; c < n; ++c) {
      x[c] += alpha * p[c];
      r[c] -= alpha * Ap[c];
    }
    if (singular) {
      removeMean(fluid, nFluid, r);
    }
    if (std::sqrt(dot(r, r)) <= tol_ * bnorm) {
      ++it;
      rep.converged = true;
      break;
    }
    for (std::size_t c = 0; c < n; ++c) {
      z[c] = fluid[c] ? r[c] / diag[c] : 0.0;
    }
    if (singular) {
      removeMean(fluid, nFluid, z);
    }
    const double rzNew = dot(r, z);
    const double beta = rzNew / rz;
    for (std::size_t c = 0; c < n; ++c) {
      p[c] = z[c] + beta * p[c];
    }
    rz = rzNew;
  }

  if (singular) {
    removeMean(fluid, nFluid, x);
  }
  applyA(tree, x, Ap);
  double rr = 0.0;
  for (std::size_t c = 0; c < n; ++c) {
    const double e = rhs[c] - Ap[c];
    rr += e * e;
  }
  rep.iterations = it;
  rep.residual = std::sqrt(rr) / bnorm;
  rep.converged = rep.converged || rep.residual <= 1e-6;
  return rep;
}

}  // namespace nabla::flow
