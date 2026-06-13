#pragma once

#include <cstddef>

namespace nabla {

// Simulation configuration. The backend serializes a validated request to a
// `key = value` spec file (file-based IPC) which the core deserializes into
// this struct — see nabla::io::read_config in io.hpp.
struct SimConfig {
  std::size_t nx = 64;          // grid cells in x
  std::size_t ny = 64;          // grid cells in y
  int steps = 100;              // number of explicit time steps
  double diffusivity = 0.1;     // thermal diffusivity (alpha)
  double dt = 0.1;              // time-step size
  double dx = 1.0;              // uniform grid spacing
  double boundary_temp = 0.0;   // Dirichlet boundary value
  double initial_temp = 1.0;    // initial interior value
};

}  // namespace nabla
