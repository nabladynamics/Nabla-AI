#pragma once

#include <cstddef>
#include <string>

#include "nabla/adaptive/adaptive.hpp"

namespace nabla::adaptive {

// SolverBackend hides how the flow is advanced from the control plane. The
// shipped backend wraps the validated Prompt-4 baseline; an octree-native NS
// solve can replace it behind this seam (ADR-0004).
class SolverBackend {
 public:
  virtual ~SolverBackend() = default;
  virtual void step() = 0;
  virtual double sample(int component, Vec3 p) const = 0;  // 0=u,1=v,2=w,3=p
  virtual double cd() const = 0;
  virtual double cl() const = 0;
  virtual double momentumResidual() const = 0;
  virtual double nu() const = 0;
  virtual std::size_t uniformCells() const = 0;
  // Per-step telemetry (drives the live diagnostics stream).
  virtual double t() const = 0;
  virtual double dt() const = 0;
  virtual double cfl() const = 0;
  virtual double continuityResidual() const = 0;
  virtual double divergenceMax() const = 0;
  virtual double massError() const = 0;
  virtual int poissonIters() const = 0;
  virtual double poissonResidual() const = 0;
};

struct AdaptiveRunOptions {
  int resolution = 6;       // baseline cells per cube height h
  double reynolds = 500.0;  // Re_h
  int octreeBaseLevel = 3;
  int octreeMaxLevel = 6;
  int warmupSteps = 25;     // baseline-only steps to develop the flow
  int adaptiveSteps = 20;   // control-plane steps
  std::string runDir = "run_adapt";
};

struct AdaptiveRunResult {
  double cd = 0.0;
  double cl = 0.0;
  std::size_t adaptiveCells = 0;
  std::size_t uniformFineCells = 0;
  double cellSpeedup = 0.0;
  AuditTrail::Totals audit;
  std::string runDir;
};

AdaptiveRunResult runAdaptiveCube(const AdaptiveRunOptions& opt);

}  // namespace nabla::adaptive
