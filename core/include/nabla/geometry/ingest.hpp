#pragma once

#include <string>

namespace nabla::geometry {

// Options for Stage-1 ingestion (STL -> meshed domain + geometry report).
struct IngestOptions {
  std::string caseName = "wall-mounted-cube";
  std::string vtuPath;     // default: <stem>.vtu
  std::string reportPath;  // default: <stem>.geometry.json

  int baseLevel = 3;            // uniform far-field refinement
  int maxLevel = 11;           // octree capacity cap for this run
  double resolution = 8.0;     // surface cells across the feature scale (h)
  int edgeExtraLevels = 2;     // extra refinement levels at sharp edges
  double sharpAngleDeg = 30.0; // dihedral threshold for "sharp"
};

// Summary returned to the CLI.
struct IngestSummary {
  std::size_t cells = 0;
  std::size_t insideSolid = 0;
  std::size_t cut = 0;
  std::size_t fluid = 0;
  int minLevel = 0;
  int maxLevel = 0;
  bool balanced = false;
  bool watertight = false;
  std::size_t sharpEdges = 0;
  std::size_t corners = 0;
  double cubeHeight = 0.0;
  std::string vtuPath;
  std::string reportPath;
};

// Run the full Stage-1 pipeline. Throws std::runtime_error on unrecoverable
// errors (e.g., unreadable STL). Writes the .vtu and JSON report as a side
// effect.
IngestSummary runIngest(const std::string& stlPath, const IngestOptions& options);

}  // namespace nabla::geometry
