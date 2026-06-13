#pragma once

#include <cstddef>
#include <string>

#include "nabla/geometry/features.hpp"
#include "nabla/geometry/stl_reader.hpp"
#include "nabla/geometry/trimesh.hpp"

namespace nabla::geometry {

struct DomainInfo {
  std::string caseName;
  Vec3 origin{};
  Vec3 extent{};
  double cubeHeight = 0.0;
};

struct MeshStats {
  std::size_t cells = 0;
  std::size_t insideSolid = 0;
  std::size_t cut = 0;
  std::size_t fluid = 0;
  int minLevel = 0;
  int maxLevel = 0;
  bool balanced = false;
  double surfaceTarget = 0.0;
  double edgeTarget = 0.0;
};

// Write the machine-readable geometry report consumed by the backend AI layer.
void writeGeometryReport(const std::string& path, const std::string& stlPath,
                         const StlReadResult& stl, const FeatureSet& features,
                         const DomainInfo& domain, const MeshStats& stats);

}  // namespace nabla::geometry
