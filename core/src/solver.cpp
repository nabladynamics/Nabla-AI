#include "nabla/solver.hpp"

#include <utility>

namespace nabla {

DiffusionSolver::DiffusionSolver(SimConfig config) : config_(config) {}

double DiffusionSolver::stability_number() const {
  return config_.diffusivity * config_.dt / (config_.dx * config_.dx);
}

SolveResult DiffusionSolver::solve(Field2D& field) const {
  const std::size_t nx = field.nx();
  const std::size_t ny = field.ny();
  const double r = stability_number();

  // Double-buffer: `next` starts as a copy of `field` so its (fixed) boundary
  // cells already hold the Dirichlet values and are never overwritten below.
  Field2D next = field;

  int step = 0;
  for (; step < config_.steps; ++step) {
    for (std::size_t j = 1; j + 1 < ny; ++j) {
      for (std::size_t i = 1; i + 1 < nx; ++i) {
        const double center = field.at(i, j);
        const double laplacian = field.at(i + 1, j) + field.at(i - 1, j) +
                                 field.at(i, j + 1) + field.at(i, j - 1) -
                                 4.0 * center;
        next.at(i, j) = center + r * laplacian;
      }
    }
    std::swap(field, next);
  }

  SolveResult result;
  result.steps_run = step;
  result.stability = r;
  result.stable = r <= kStableLimit;
  result.final_max = field.max();
  result.final_mean = field.mean();
  return result;
}

}  // namespace nabla
