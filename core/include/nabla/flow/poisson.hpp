#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nabla/flow/grid.hpp"

namespace nabla::flow {

// Pressure-Poisson interface. The fractional-step solver depends only on this,
// so a sparse-direct (Eigen SparseLU) or geometric-multigrid implementation can
// replace the default CG without touching the NS core (see ADR-0003).
//
// All implementations solve  A x = b  with A = -Laplacian discretized on the
// grid's BCs (SPD): zero-Neumann at walls/inlet and fluid-solid faces, Dirichlet
// x=0 at outlets, periodic wrap on periodic axes. Pure-Neumann systems are
// singular; the RHS is projected onto the compatible subspace and the constant
// null-space mode is removed from the result.
class PoissonSolver {
 public:
  virtual ~PoissonSolver() = default;
  struct Report {
    int iterations = 0;
    double residual = 0.0;
    bool converged = false;
  };
  // x and b are compact interior arrays of length grid.cellCount().
  virtual Report solve(const UniformGrid& grid, const std::vector<double>& b,
                       std::vector<double>& x) = 0;
  [[nodiscard]] virtual std::string name() const = 0;
};

// Jacobi-preconditioned conjugate gradient. Matrix-free: the Laplacian action is
// evaluated directly from the grid + masks, so there is no assembled matrix.
class CgPoisson : public PoissonSolver {
 public:
  explicit CgPoisson(double tol = 1e-10, int maxIters = 5000)
      : tol_(tol), maxIters_(maxIters) {}

  Report solve(const UniformGrid& grid, const std::vector<double>& b,
               std::vector<double>& x) override;
  [[nodiscard]] std::string name() const override { return "cg-jacobi"; }

  // A*x for the discrete -Laplacian (exposed for tests + the multigrid solver,
  // so operator and smoother share one stencil definition).
  static void applyA(const UniformGrid& grid, const std::vector<double>& x,
                     std::vector<double>& out);
  static bool hasDirichlet(const UniformGrid& grid);
  // Main-diagonal of A (sum of active face coefficients); solid cells -> 1.
  // Used by the multigrid smoother. Consistent with applyA by construction.
  static void buildDiagonal(const UniformGrid& grid, std::vector<double>& diag);

 private:
  double tol_;
  int maxIters_;
};

// Geometric multigrid for the pressure Poisson, run as the preconditioner for
// CG (MG-PCG) for robustness with the immersed solid and mixed Neumann/Dirichlet
// BCs. The multigrid hierarchy is built by 2:1 agglomeration of the cell grid
// (a coarse cell is the parent of up to 2x2x2 fine cells) — the same parent/
// child relationship the octree uses, so this carries over to AMR meshes
// (ADR-0007). Components: damped-Jacobi smoother (shares CgPoisson::applyA, so
// operator and smoother never drift), volume-weighted full-weighting
// restriction, piecewise-constant (conservative) prolongation, and a smoothed
// near-exact solve on the coarsest level. O(N) per solve and rebuild-cheap when
// the mesh adapts.
class MgPoisson : public PoissonSolver {
 public:
  explicit MgPoisson(double tol = 1e-9, int maxCycles = 100, int preSmooth = 2,
                     int postSmooth = 2)
      : tol_(tol), maxCycles_(maxCycles), pre_(preSmooth), post_(postSmooth) {}

  Report solve(const UniformGrid& grid, const std::vector<double>& b,
               std::vector<double>& x) override;
  [[nodiscard]] std::string name() const override { return "mg-pcg"; }

  // Defined in multigrid.cpp; public so the file-local cycle helpers can name
  // them. Opaque to every other translation unit.
  struct Level;      // one grid in the hierarchy + its diagonal/fluid metadata
  struct Hierarchy;  // owns the levels and the null-space flag

 private:
  double tol_;
  int maxCycles_;
  int pre_, post_;
  std::shared_ptr<Hierarchy> cache_;  // rebuilt only when the grid changes
};

}  // namespace nabla::flow
