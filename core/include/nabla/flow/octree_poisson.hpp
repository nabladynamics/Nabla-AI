#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "nabla/mesh/octree.hpp"

namespace nabla::flow {

// Cell-centered finite-volume pressure Poisson on the octree leaves — the first
// brick of the octree-native flow solve (ADR-0008).
//
// The operator is the SPD, volume-integrated negative Laplacian
//
//     (A p)_i = sum over faces f of  coeff_f * (p_i - p_n)
//
// with coeff_f = area_f / dist_f, where area_f is the *finer* cell's transverse
// face area and dist_f the true centre-to-centre distance along the face
// normal. Because a coarse cell and its (up to 4) fine neighbours across a 2:1
// jump share the same area_f and dist_f, the flux a fine cell contributes is the
// exact negative of what the coarse cell receives — the discrete operator is
// **conservative across level jumps** (verified by a sum-to-zero test). Faces on
// the domain boundary and faces touching a non-fluid (solid/cut) cell are
// homogeneous Neumann (skipped). A pure-Neumann domain is singular; the constant
// null-space mode is projected out of the RHS and the solution.
//
// `fluid` cells are mask == Fluid; solid/cut cells carry an identity row.
class OctreePoisson {
 public:
  explicit OctreePoisson(double tol = 1e-9, int maxIters = 2000)
      : tol_(tol), maxIters_(maxIters) {}

  struct Report {
    int iterations = 0;
    double residual = 0.0;
    bool converged = false;
  };

  // Solve A x = b over the fluid cells. x and b are parallel to tree cells.
  Report solve(const mesh::Octree& tree, const std::vector<double>& b,
               std::vector<double>& x);

  // Matrix-free operator action (exposed for tests + the eventual multigrid).
  static void applyA(const mesh::Octree& tree, const std::vector<double>& x,
                     std::vector<double>& out);
  // Main diagonal (sum of active face coefficients); non-fluid cells -> 1.
  static void buildDiagonal(const mesh::Octree& tree, std::vector<double>& diag);

  [[nodiscard]] std::string name() const { return "octree-cg-jacobi"; }

 private:
  double tol_;
  int maxIters_;
};

}  // namespace nabla::flow
