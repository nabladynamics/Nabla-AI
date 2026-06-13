#pragma once

#include <cstdint>
#include <vector>

#include "nabla/geometry/trimesh.hpp"

namespace nabla::geometry {

// Separating-axis test: does triangle (a,b,c) overlap the AABB with the given
// center and half-extents? Touching counts as overlap. Exposed for testing.
bool triBoxOverlap(Vec3 boxCenter, Vec3 boxHalf, Vec3 a, Vec3 b, Vec3 c);

// Spatial query structure over a triangle soup: a binary BVH used for fast
// "does any triangle cross this cell box?" (-> CUT) and ray-parity point-in-
// solid (-> INSIDE/OUTSIDE) queries.
class SolidGeometry {
 public:
  explicit SolidGeometry(const TriMesh& mesh);

  // Any triangle overlapping the axis-aligned box [lo, hi]?  (CUT test)
  [[nodiscard]] bool intersectsBox(Vec3 lo, Vec3 hi) const;

  // Is point p inside the (assumed closed) surface? Ray-parity, majority of 3
  // generic directions for robustness against edge/vertex grazing.
  [[nodiscard]] bool pointInside(Vec3 p) const;

  [[nodiscard]] const TriMesh& mesh() const { return mesh_; }

 private:
  struct Node {
    Vec3 lo, hi;
    uint32_t start = 0, count = 0;  // range into tri_ (leaf) ...
    int32_t left = -1, right = -1;  // ... or children (internal)
  };

  int buildRange(std::size_t begin, std::size_t end, std::vector<Vec3>& cen);
  int countRayHits(Vec3 o, Vec3 d) const;

  TriMesh mesh_;
  std::vector<Vec3> v0_, v1_, v2_;  // SoA triangle vertices
  std::vector<Vec3> triLo_, triHi_;
  std::vector<uint32_t> tri_;       // triangle order (BVH leaves index this)
  std::vector<Node> nodes_;
};

}  // namespace nabla::geometry
