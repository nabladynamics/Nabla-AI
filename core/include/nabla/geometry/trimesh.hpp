#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nabla/mesh/octree.hpp"  // reuse mesh::Vec3 for octree interop

namespace nabla::geometry {

using Vec3 = mesh::Vec3;
using Tri = std::array<uint32_t, 3>;

// --- minimal Vec3 algebra (ADL-found within nabla::geometry) ---------------
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(double s, Vec3 a) { return a * s; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double norm(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalized(Vec3 a) {
  const double n = norm(a);
  return n > 0.0 ? a * (1.0 / n) : a;
}
inline Vec3 vmin(Vec3 a, Vec3 b) {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
inline Vec3 vmax(Vec3 a, Vec3 b) {
  return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
inline double comp(Vec3 v, int axis) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }

struct Aabb {
  Vec3 lo;
  Vec3 hi;
};

// Indexed triangle surface mesh.
struct TriMesh {
  std::vector<Vec3> vertices;
  std::vector<Tri> tris;

  [[nodiscard]] std::size_t triangleCount() const { return tris.size(); }
  [[nodiscard]] std::size_t vertexCount() const { return vertices.size(); }

  [[nodiscard]] Vec3 faceNormal(std::size_t f) const {
    const Vec3& a = vertices[tris[f][0]];
    const Vec3& b = vertices[tris[f][1]];
    const Vec3& c = vertices[tris[f][2]];
    return normalized(cross(b - a, c - a));
  }

  [[nodiscard]] double faceArea(std::size_t f) const {
    const Vec3& a = vertices[tris[f][0]];
    const Vec3& b = vertices[tris[f][1]];
    const Vec3& c = vertices[tris[f][2]];
    return 0.5 * norm(cross(b - a, c - a));
  }

  [[nodiscard]] Aabb boundingBox() const {
    if (vertices.empty()) {
      return {{0, 0, 0}, {0, 0, 0}};
    }
    Vec3 lo = vertices[0];
    Vec3 hi = vertices[0];
    for (const Vec3& v : vertices) {
      lo = vmin(lo, v);
      hi = vmax(hi, v);
    }
    return {lo, hi};
  }

  void translate(Vec3 shift) {
    for (Vec3& v : vertices) {
      v = v + shift;
    }
  }
};

// Axis-aligned box as a 12-triangle watertight mesh (outward-ish winding; our
// uses — ray parity, |dihedral|, box overlap — are orientation-insensitive).
inline TriMesh makeBox(Vec3 lo, Vec3 hi) {
  TriMesh m;
  m.vertices = {
      {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z},
      {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z},
  };
  m.tris = {
      {0, 4, 6}, {0, 6, 2},  // -x
      {1, 5, 7}, {1, 7, 3},  // +x
      {0, 1, 5}, {0, 5, 4},  // -y
      {2, 3, 7}, {2, 7, 6},  // +y
      {0, 2, 3}, {0, 3, 1},  // -z
      {4, 5, 7}, {4, 7, 6},  // +z
  };
  return m;
}

}  // namespace nabla::geometry
