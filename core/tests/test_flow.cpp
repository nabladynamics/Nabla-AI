#include <cmath>
#include <cstddef>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
}

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nabla/flow/case.hpp"
#include "nabla/flow/ns_solver.hpp"
#include "nabla/flow/poisson.hpp"

using namespace nabla::flow;

TEST_CASE("Cg Poisson recovers a manufactured periodic solution", "[flow][poisson]") {
  UniformGrid g;
  g.nx = 16;
  g.ny = 16;
  g.nz = 1;
  g.dx = g.dy = g.dz = 1.0 / 16;
  for (auto& f : g.face) {
    f.type = BoundaryType::Periodic;
  }
  g.allocate();

  // phi = cos(2pi x) cos(2pi y); b = A phi (A = -Laplacian).
  std::vector<double> phi(g.cellCount()), b;
  for (int j = 0; j < g.ny; ++j) {
    for (int i = 0; i < g.nx; ++i) {
      const double x = (i + 0.5) * g.dx, y = (j + 0.5) * g.dy;
      phi[g.cidx(i, j, 0)] = std::cos(2 * kPi * x) * std::cos(2 * kPi * y);
    }
  }
  CgPoisson::applyA(g, phi, b);

  CgPoisson solver(1e-12, 5000);
  std::vector<double> x;
  const PoissonSolver::Report rep = solver.solve(g, b, x);
  REQUIRE(rep.residual < 1e-8);

  // Compare up to the additive constant (periodic null space).
  double mp = 0, mx = 0;
  for (std::size_t c = 0; c < phi.size(); ++c) {
    mp += phi[c];
    mx += x[c];
  }
  mp /= static_cast<double>(phi.size());
  mx /= static_cast<double>(x.size());
  double err = 0;
  for (std::size_t c = 0; c < phi.size(); ++c) {
    const double e = (x[c] - mx) - (phi[c] - mp);
    err += e * e;
  }
  REQUIRE(std::sqrt(err / static_cast<double>(phi.size())) < 1e-6);
}

TEST_CASE("MG-PCG matches CG and converges in few cycles with an immersed solid",
          "[flow][poisson][multigrid]") {
  // 3-D channel-like operator: inlet / outlet (non-singular) + walls + periodic
  // span, with a solid block — exercises the immersed-boundary closure on every
  // coarse level of the agglomeration hierarchy.
  UniformGrid g;
  g.nx = 32;
  g.ny = 16;
  g.nz = 16;
  g.dx = g.dy = g.dz = 1.0 / 16;
  g.face[0].type = BoundaryType::Inlet;
  g.face[1].type = BoundaryType::Outlet;
  g.face[2].type = BoundaryType::Wall;
  g.face[3].type = BoundaryType::Wall;
  g.face[4].type = BoundaryType::Periodic;
  g.face[5].type = BoundaryType::Periodic;
  g.allocate();
  for (int k = 6; k < 10; ++k) {
    for (int j = 0; j < 6; ++j) {
      for (int i = 12; i < 18; ++i) {
        g.solid[g.cidx(i, j, k)] = 1;  // wall-mounted block
      }
    }
  }

  // Manufactured RHS: b = A x_exact for a random fluid field (0 in the solid).
  std::mt19937 rng(12345);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  std::vector<double> xExact(g.cellCount(), 0.0), b;
  for (int k = 0; k < g.nz; ++k) {
    for (int j = 0; j < g.ny; ++j) {
      for (int i = 0; i < g.nx; ++i) {
        if (!g.isSolid(i, j, k)) {
          xExact[g.cidx(i, j, k)] = dist(rng);
        }
      }
    }
  }
  CgPoisson::applyA(g, xExact, b);

  MgPoisson mg(1e-10, 100);
  std::vector<double> x;
  const PoissonSolver::Report rep = mg.solve(g, b, x);

  REQUIRE(rep.converged);
  REQUIRE(rep.residual < 1e-8);
  REQUIRE(rep.iterations <= 50);  // mesh-size-independent; ~15 in practice

  double err = 0.0;
  for (std::size_t c = 0; c < xExact.size(); ++c) {
    const double e = x[c] - xExact[c];
    err += e * e;
  }
  REQUIRE(std::sqrt(err / static_cast<double>(xExact.size())) < 1e-6);
}

TEST_CASE("MG-PCG recovers a manufactured periodic solution (singular null space)",
          "[flow][poisson][multigrid]") {
  UniformGrid g;
  g.nx = 24;
  g.ny = 24;
  g.nz = 1;
  g.dx = g.dy = g.dz = 1.0 / 24;
  for (auto& f : g.face) {
    f.type = BoundaryType::Periodic;
  }
  g.allocate();

  std::vector<double> phi(g.cellCount()), b;
  for (int j = 0; j < g.ny; ++j) {
    for (int i = 0; i < g.nx; ++i) {
      const double x = (i + 0.5) * g.dx, y = (j + 0.5) * g.dy;
      phi[g.cidx(i, j, 0)] = std::cos(2 * kPi * x) * std::cos(2 * kPi * y);
    }
  }
  CgPoisson::applyA(g, phi, b);

  MgPoisson mg(1e-12, 100);
  std::vector<double> x;
  const PoissonSolver::Report rep = mg.solve(g, b, x);
  REQUIRE(rep.residual < 1e-8);
  REQUIRE(rep.iterations <= 50);

  double mp = 0, mx = 0;
  for (std::size_t c = 0; c < phi.size(); ++c) {
    mp += phi[c];
    mx += x[c];
  }
  mp /= static_cast<double>(phi.size());
  mx /= static_cast<double>(x.size());
  double err = 0;
  for (std::size_t c = 0; c < phi.size(); ++c) {
    const double e = (x[c] - mx) - (phi[c] - mp);
    err += e * e;
  }
  REQUIRE(std::sqrt(err / static_cast<double>(phi.size())) < 1e-6);
}

TEST_CASE("projection drives the velocity field divergence-free", "[flow][projection]") {
  CaseSpec sp = makeLidCavity(24, 100.0, 1.0);
  sp.flow.poissonTol = 1e-10;
  NSSolver s(sp.grid, sp.flow);
  s.initialize();
  StepReport r;
  for (int i = 0; i < 5; ++i) {
    r = s.step();
  }
  // continuity residual = ||div u|| after projection must be ~Poisson tol.
  REQUIRE(r.continuityResidual < 1e-7);
}

TEST_CASE("Poiseuille channel reproduces the analytic parabola", "[flow][validation]") {
  const int nx = 64, ny = 24;
  CaseSpec sp = makeChannel(nx, ny, 100.0, 1.0);
  sp.flow.poissonTol = 1e-9;
  NSSolver s(sp.grid, sp.flow);
  s.initialize();
  for (int i = 0; i < 1200; ++i) {
    s.step();
  }
  double num = 0, den = 0;
  for (int j = 0; j < ny; ++j) {
    const double y = (j + 0.5) / ny;
    const double exact = 1.5 * (1.0 - (2 * (y - 0.5)) * (2 * (y - 0.5)));
    const double got = s.cellU(nx - 1, j, 0);
    num += (got - exact) * (got - exact);
    den += exact * exact;
  }
  const double l2 = std::sqrt(num / den);
  REQUIRE(l2 < 0.02);  // <= 2% L2
}

TEST_CASE("WENO-5 convection runs stably and stays bounded", "[flow][weno]") {
  CaseSpec sp = makeChannel(48, 16, 200.0, 1.0);
  sp.flow.convection = ConvectionScheme::Weno5;
  sp.flow.poissonTol = 1e-8;
  NSSolver s(sp.grid, sp.flow);
  s.initialize();
  StepReport r;
  for (int i = 0; i < 400; ++i) {
    r = s.step();
  }
  REQUIRE(std::isfinite(r.momentumResidual));
  REQUIRE(std::isfinite(r.continuityResidual));
  // No overshoot beyond the parabolic peak (1.5*Ubulk) by more than a little.
  for (int j = 0; j < 16; ++j) {
    REQUIRE(s.cellU(40, j, 0) < 1.7);
    REQUIRE(s.cellU(40, j, 0) > -0.1);
  }
}

TEST_CASE("checkpoint gives bit-exact restart", "[flow][restart]") {
  CaseSpec sp = makeChannel(32, 16, 100.0, 1.0);
  sp.flow.poissonTol = 1e-9;

  // Reference trajectory: 8 steps.
  NSSolver ref(sp.grid, sp.flow);
  ref.initialize();
  for (int i = 0; i < 8; ++i) {
    ref.step();
  }
  const std::vector<double> goldU = ref.uFace();
  const std::vector<double> goldP = ref.pressure();

  // Run 4 steps, checkpoint, restart in a fresh solver, run 4 more.
  NSSolver a(sp.grid, sp.flow);
  a.initialize();
  for (int i = 0; i < 4; ++i) {
    a.step();
  }
  const std::string path =
      (std::filesystem::temp_directory_path() / "nabla_flow_restart.ckpt").string();
  writeCheckpoint(a, path);

  NSSolver b(sp.grid, sp.flow);
  b.initialize();
  readCheckpoint(b, path);
  for (int i = 0; i < 4; ++i) {
    b.step();
  }
  std::filesystem::remove(path);

  REQUIRE(b.uFace() == goldU);       // bit-exact
  REQUIRE(b.pressure() == goldP);
}
