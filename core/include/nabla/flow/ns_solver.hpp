#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "nabla/flow/grid.hpp"
#include "nabla/flow/poisson.hpp"

namespace nabla::flow {

enum class ConvectionScheme { Central, Weno5 };

struct FlowConfig {
  double reynolds = 100.0;      // Re based on velocityScale and refLength
  double refLength = 1.0;       // L used in Re = U L / nu
  double velocityScale = 1.0;   // U used for Re, CFL, and force coefficients
  double density = 1.0;
  double cfl = 0.8;
  V3 bodyForce{0.0, 0.0, 0.0};
  ConvectionScheme convection = ConvectionScheme::Central;

  double poissonTol = 1e-9;
  int poissonMaxIters = 5000;

  // convergence-gate tolerances
  double tolContinuity = 1e-7;
  double tolMass = 1e-8;

  // force-coefficient reference area (frontal area for the cube). 0 => auto.
  double refArea = 0.0;
};

struct StepReport {
  int step = 0;
  double t = 0.0;
  double dt = 0.0;
  double cfl = 0.0;
  double momentumResidual = 0.0;   // ||u^{n+1}-u^n|| / (dt * U)
  double continuityResidual = 0.0; // L2 ||div u|| after projection
  double divergenceMax = 0.0;      // max |div u| after projection (worst cell)
  double massError = 0.0;          // net boundary mass flux / (U * A)
  int poissonIters = 0;            // total pressure-solver cycles over the 3 RK3 stages
  double poissonResidual = 0.0;    // worst per-solve relative residual this step
  double cd = 0.0;
  double cl = 0.0;
  std::size_t cellCount = 0;
  bool accepted = false;
};

// Staggered (MAC) incompressible Navier–Stokes solver on a uniform grid:
//  - velocities on cell faces (u at x-faces, v at y-faces, w at z-faces)
//  - pressure at cell centers
//  - SSP-RK3 fractional-step (projection) time integration
//  - 2nd-order central diffusion; central or WENO-5 convection
//  - pressure projection via the injected PoissonSolver
// nz == 1 with periodic z is a clean 2D problem.
class NSSolver {
 public:
  NSSolver(UniformGrid grid, FlowConfig config,
           std::shared_ptr<PoissonSolver> poisson = nullptr);

  void initialize();          // zero interior, apply inlet as initial guess
  StepReport step();          // advance one step; dt chosen from CFL
  StepReport step(double dt); // advance with a fixed dt (used for restart parity)

  [[nodiscard]] double time() const { return t_; }
  [[nodiscard]] int stepIndex() const { return step_; }
  [[nodiscard]] double dtLast() const { return dtLast_; }
  [[nodiscard]] double nu() const { return nu_; }
  [[nodiscard]] const UniformGrid& grid() const { return grid_; }
  [[nodiscard]] const FlowConfig& config() const { return cfg_; }

  // Cell-centered fields (compact interior, length grid.cellCount()) for output.
  [[nodiscard]] std::vector<double> cellVelocity(int component) const;
  [[nodiscard]] std::vector<double> cellPressure() const { return p_; }
  [[nodiscard]] double cellU(int i, int j, int k) const;  // interpolated to center
  [[nodiscard]] double cellV(int i, int j, int k) const;
  [[nodiscard]] double cellW(int i, int j, int k) const;

  // Raw staggered state (for checkpoint exact round-trip).
  [[nodiscard]] const std::vector<double>& uFace() const { return u_; }
  [[nodiscard]] const std::vector<double>& vFace() const { return v_; }
  [[nodiscard]] const std::vector<double>& wFace() const { return w_; }
  [[nodiscard]] const std::vector<double>& pressure() const { return p_; }
  void setState(std::vector<double> u, std::vector<double> v, std::vector<double> w,
                std::vector<double> p, int step, double t, double dtLast);

  [[nodiscard]] double computeDt() const;

 private:
  // staggered index helpers
  [[nodiscard]] std::size_t ui(int i, int j, int k) const {
    return static_cast<std::size_t>(i) +
           static_cast<std::size_t>(grid_.nx + 1) *
               (static_cast<std::size_t>(j) +
                static_cast<std::size_t>(grid_.ny) * static_cast<std::size_t>(k));
  }
  [[nodiscard]] std::size_t vi(int i, int j, int k) const {
    return static_cast<std::size_t>(i) +
           static_cast<std::size_t>(grid_.nx) *
               (static_cast<std::size_t>(j) +
                static_cast<std::size_t>(grid_.ny + 1) * static_cast<std::size_t>(k));
  }
  [[nodiscard]] std::size_t wi(int i, int j, int k) const {
    return static_cast<std::size_t>(i) +
           static_cast<std::size_t>(grid_.nx) *
               (static_cast<std::size_t>(j) +
                static_cast<std::size_t>(grid_.ny) * static_cast<std::size_t>(k));
  }
  [[nodiscard]] std::size_t ci(int i, int j, int k) const { return grid_.cidx(i, j, k); }

  // BC-aware staggered accessors (handle ghosts via reflection / wrap / inlet).
  [[nodiscard]] double U(const std::vector<double>& u, int i, int j, int k) const;
  [[nodiscard]] double Vv(const std::vector<double>& v, int i, int j, int k) const;
  [[nodiscard]] double W(const std::vector<double>& w, int i, int j, int k) const;

  // Apply boundary velocities. `enforceOutflow` controls the convective/zero-
  // gradient outlet copy: true in the predictor (so the divergence RHS sees a
  // physical outflow), but false right after the pressure projection — there
  // the projection has already set the divergence-free outlet face, and copying
  // the interior value back over it re-injects divergence at the outflow column
  // (the cause of the rising continuity residual). Walls/inlet/solid/periodic
  // are idempotent post-projection and always enforced.
  void applyVelocityBC(std::vector<double>& u, std::vector<double>& v,
                       std::vector<double>& w, bool enforceOutflow = true) const;
  void computeRhs(const std::vector<double>& u, const std::vector<double>& v,
                  const std::vector<double>& w, std::vector<double>& ru,
                  std::vector<double>& rv, std::vector<double>& rw) const;
  void project(std::vector<double>& u, std::vector<double>& v, std::vector<double>& w,
               double dt, int& poissonIters, double& poissonRes);
  [[nodiscard]] double divergenceL2(const std::vector<double>& u,
                                    const std::vector<double>& v,
                                    const std::vector<double>& w) const;
  [[nodiscard]] double divergenceMax(const std::vector<double>& u,
                                     const std::vector<double>& v,
                                     const std::vector<double>& w) const;
  [[nodiscard]] double massFluxError(const std::vector<double>& u,
                                     const std::vector<double>& v,
                                     const std::vector<double>& w) const;
  void computeForces(double& cd, double& cl) const;
  [[nodiscard]] V3 inletVelocityAt(int faceId, int j, int k) const;

  UniformGrid grid_;
  FlowConfig cfg_;
  double nu_;
  std::shared_ptr<PoissonSolver> poisson_;

  std::vector<double> u_, v_, w_;  // staggered velocities
  std::vector<double> p_;          // cell-centered pressure
  bool hasSolid_ = false;

  int step_ = 0;
  double t_ = 0.0;
  double dtLast_ = 0.0;
};

}  // namespace nabla::flow
