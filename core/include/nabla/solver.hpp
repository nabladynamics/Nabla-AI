#pragma once

#include "nabla/config.hpp"
#include "nabla/field.hpp"

namespace nabla {

struct SolveResult {
  int steps_run = 0;
  double final_max = 0.0;
  double final_mean = 0.0;
  double stability = 0.0;  // CFL-like number alpha*dt/dx^2
  bool stable = false;     // true when stability <= kStableLimit
};

// Explicit (forward-Euler) finite-difference solver for the 2D heat/diffusion
// equation with fixed Dirichlet boundaries. This is the Phase-0 reference
// kernel; the "physics-adaptive" model selection lives one layer up, in the
// orchestration backend, which chooses which kernel/config to dispatch.
class DiffusionSolver {
 public:
  // Stability limit for the 2D explicit 5-point stencil: alpha*dt/dx^2 <= 1/4.
  static constexpr double kStableLimit = 0.25;

  explicit DiffusionSolver(SimConfig config);

  // Advances `field` in place by config.steps time steps. Boundary cells are
  // treated as fixed (Dirichlet) and are never written.
  [[nodiscard]] SolveResult solve(Field2D& field) const;

  // CFL-like stability number for the explicit scheme.
  [[nodiscard]] double stability_number() const;

  [[nodiscard]] const SimConfig& config() const noexcept { return config_; }

 private:
  SimConfig config_;
};

}  // namespace nabla
