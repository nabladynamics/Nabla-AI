#pragma once

#include <cstddef>
#include <vector>

#include "nabla/geometry/trimesh.hpp"

namespace nabla::geometry {

struct SharpEdge {
  Vec3 a;
  Vec3 b;
  double dihedralDeg = 0.0;  // angle between the two adjacent face normals
  double length = 0.0;
};

// Geometric features extracted from a (cleaned, welded) surface mesh. The
// backend AI layer consumes the JSON form of this to identify the object and
// propose experiments.
struct FeatureSet {
  std::vector<SharpEdge> sharpEdges;
  std::vector<Vec3> corners;        // vertices where >= 3 sharp edges meet
  std::vector<double> faceCurvature; // per-triangle curvature estimate (1/length)

  double sharpAngleDeg = 0.0;       // threshold used
  double minSharpEdgeLength = 0.0;
  double maxSharpEdgeLength = 0.0;
  double totalSharpEdgeLength = 0.0;
  double curvatureMin = 0.0;
  double curvatureMax = 0.0;
  double curvatureMean = 0.0;
  double smallestFeature = 0.0;     // size of the smallest feature to resolve
};

// Detect features. An edge is "sharp" when the dihedral angle between its two
// incident faces exceeds `sharpAngleDeg`.
FeatureSet detectFeatures(const TriMesh& mesh, double sharpAngleDeg = 30.0);

}  // namespace nabla::geometry
