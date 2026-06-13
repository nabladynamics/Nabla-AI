#pragma once

#include <cstddef>
#include <string>

#include "nabla/flow/grid.hpp"
#include "nabla/flow/ns_solver.hpp"

namespace nabla::flow {

struct RunOptions {
  std::string runDir = "run";
  int maxSteps = 20000;
  double stopTime = 0.0;     // 0 => ignore
  double steadyTol = 0.0;    // stop when momentum residual < steadyTol (0 => ignore)
  int snapshotEvery = 0;     // .vtu every N steps (0 => only final)
  int checkpointEvery = 0;   // checkpoint every M steps (0 => only final)
  std::string restartFrom;   // checkpoint path to resume from
  bool quiet = false;
};

struct CaseSpec {
  std::string name = "case";
  UniformGrid grid;
  FlowConfig flow;
  RunOptions run;
};

struct RunSummary {
  int steps = 0;
  double time = 0.0;
  double momentumResidual = 0.0;
  double continuityResidual = 0.0;
  double massError = 0.0;
  double cd = 0.0;
  double cl = 0.0;
  std::size_t cells = 0;
  bool steady = false;
  std::string runDir;
};

// Case builders (also used directly by the validation tests).
CaseSpec makeLidCavity(int n, double reynolds, double lidVelocity = 1.0);
CaseSpec makeChannel(int nx, int ny, double reynolds, double bulkVelocity = 1.0);
CaseSpec makeWallMountedCube(int resolution, double reynolds);

// Load a case from a JSON description (see docs / examples/cube_case.json).
CaseSpec loadCase(const std::string& jsonPath);

// Drive a case to completion, writing diagnostics.jsonl, .vtu snapshots and
// checkpoints into run.runDir. Honors run.restartFrom for exact restart.
RunSummary runCase(const CaseSpec& spec);

// I/O helpers (exposed for tests).
void writeGridVtu(const NSSolver& solver, const std::string& path);
void writeCheckpoint(const NSSolver& solver, const std::string& path);
void readCheckpoint(NSSolver& solver, const std::string& path);

}  // namespace nabla::flow
