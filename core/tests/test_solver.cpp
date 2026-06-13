#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nabla/config.hpp"
#include "nabla/field.hpp"
#include "nabla/solver.hpp"

namespace {

// Build a field with `initial` in the interior and `boundary` on the edges.
nabla::Field2D make_field(const nabla::SimConfig& cfg) {
  nabla::Field2D field(cfg.nx, cfg.ny, cfg.initial_temp);
  for (std::size_t i = 0; i < cfg.nx; ++i) {
    field.at(i, 0) = cfg.boundary_temp;
    field.at(i, cfg.ny - 1) = cfg.boundary_temp;
  }
  for (std::size_t j = 0; j < cfg.ny; ++j) {
    field.at(0, j) = cfg.boundary_temp;
    field.at(cfg.nx - 1, j) = cfg.boundary_temp;
  }
  return field;
}

}  // namespace

TEST_CASE("stability number follows the CFL condition", "[solver]") {
  nabla::SimConfig cfg;
  cfg.diffusivity = 0.1;
  cfg.dt = 0.1;
  cfg.dx = 1.0;
  const nabla::DiffusionSolver solver(cfg);
  REQUIRE(solver.stability_number() == Catch::Approx(0.01));
}

TEST_CASE("an unstable configuration is flagged", "[solver]") {
  nabla::SimConfig cfg;
  cfg.nx = 8;
  cfg.ny = 8;
  cfg.steps = 1;
  cfg.diffusivity = 1.0;
  cfg.dt = 1.0;
  cfg.dx = 1.0;  // stability number = 1.0 > 0.25
  const nabla::DiffusionSolver solver(cfg);
  nabla::Field2D field = make_field(cfg);
  const nabla::SolveResult result = solver.solve(field);
  REQUIRE_FALSE(result.stable);
}

TEST_CASE("diffusion relaxes the interior toward the boundary", "[solver]") {
  nabla::SimConfig cfg;
  cfg.nx = 16;
  cfg.ny = 16;
  cfg.steps = 200;
  cfg.diffusivity = 0.1;
  cfg.dt = 0.1;
  cfg.dx = 1.0;
  cfg.boundary_temp = 0.0;
  cfg.initial_temp = 1.0;

  const nabla::DiffusionSolver solver(cfg);
  nabla::Field2D field = make_field(cfg);
  const double mean_before = field.mean();

  const nabla::SolveResult result = solver.solve(field);

  REQUIRE(result.stable);
  REQUIRE(result.steps_run == 200);
  // Heat diffuses out through the cold boundary: the mean must drop, and the
  // solution must respect the discrete maximum principle (no over/undershoot).
  REQUIRE(result.final_mean < mean_before);
  REQUIRE(result.final_max <= 1.0 + 1e-9);
  REQUIRE(field.min() >= 0.0 - 1e-9);
}
