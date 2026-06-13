#pragma once

#include <cstddef>
#include <string>

#include "nabla/geometry/trimesh.hpp"

namespace nabla::geometry {

// Report produced by reading + cleaning an STL. Watertightness defects are
// REPORTED, never silently repaired (a non-watertight mesh is a real modelling
// problem the AI/backend layer must know about).
struct CleanReport {
  std::size_t rawTriangles = 0;
  std::size_t rawVertices = 0;       // before welding (3 * rawTriangles)
  std::size_t weldedVertices = 0;    // vertices removed by welding
  std::size_t removedDegenerate = 0; // zero-area / repeated-index triangles
  std::size_t vertices = 0;          // after cleaning
  std::size_t triangles = 0;         // after cleaning

  bool watertight = false;
  std::size_t boundaryEdges = 0;     // edges used by exactly 1 triangle
  std::size_t nonManifoldEdges = 0;  // edges used by > 2 triangles

  Vec3 bboxLo{};
  Vec3 bboxHi{};
  double characteristicLength = 0.0;  // bounding-box diagonal
  double minEdgeLength = 0.0;
};

enum class StlFormat { Binary, Ascii };

struct StlReadResult {
  TriMesh mesh;
  CleanReport report;
  StlFormat format = StlFormat::Binary;
};

// Read + clean an STL (auto-detects binary vs ASCII).
// Throws std::runtime_error only when the file cannot be parsed at all
// (missing/garbled). Geometric defects are surfaced via CleanReport, not throws.
StlReadResult readStl(const std::string& path);

// Writers (handy for tests and round-tripping).
void writeBinaryStl(const TriMesh& mesh, const std::string& path);
void writeAsciiStl(const TriMesh& mesh, const std::string& path);

}  // namespace nabla::geometry
