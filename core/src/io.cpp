#include "nabla/io.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace nabla::io {
namespace {

std::string trim(std::string s) {
  const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

}  // namespace

SimConfig parse_config(const std::string& text) {
  SimConfig cfg;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (const auto hash = line.find('#'); hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));
    if (key.empty() || val.empty()) {
      continue;
    }

    if (key == "nx") {
      cfg.nx = static_cast<std::size_t>(std::stoul(val));
    } else if (key == "ny") {
      cfg.ny = static_cast<std::size_t>(std::stoul(val));
    } else if (key == "steps") {
      cfg.steps = std::stoi(val);
    } else if (key == "diffusivity") {
      cfg.diffusivity = std::stod(val);
    } else if (key == "dt") {
      cfg.dt = std::stod(val);
    } else if (key == "dx") {
      cfg.dx = std::stod(val);
    } else if (key == "boundary_temp") {
      cfg.boundary_temp = std::stod(val);
    } else if (key == "initial_temp") {
      cfg.initial_temp = std::stod(val);
    }
    // Unknown keys are ignored on purpose (forward compatibility).
  }
  return cfg;
}

SimConfig read_config(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("cannot open config file: " + path);
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  return parse_config(ss.str());
}

void write_result(const std::string& path, const SolveResult& result) {
  std::ofstream file(path);
  if (!file) {
    throw std::runtime_error("cannot open result file for writing: " + path);
  }
  file << "# nabla_solve result\n";
  file << "steps_run = " << result.steps_run << '\n';
  file << "final_max = " << result.final_max << '\n';
  file << "final_mean = " << result.final_mean << '\n';
  file << "stability = " << result.stability << '\n';
  file << "stable = " << (result.stable ? "true" : "false") << '\n';
}

void write_field(const std::string& path, const Field2D& field) {
  std::ofstream file(path);
  if (!file) {
    throw std::runtime_error("cannot open field file for writing: " + path);
  }
  for (std::size_t j = 0; j < field.ny(); ++j) {
    for (std::size_t i = 0; i < field.nx(); ++i) {
      file << field.at(i, j);
      if (i + 1 < field.nx()) {
        file << ' ';
      }
    }
    file << '\n';
  }
}

}  // namespace nabla::io
