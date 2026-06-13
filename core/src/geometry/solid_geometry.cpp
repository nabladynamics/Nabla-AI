#include "nabla/geometry/solid_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nabla::geometry {
namespace {
constexpr double kEps = 1e-12;
constexpr uint32_t kLeafSize = 4;
}  // namespace

// ---------------------------------------------------------------------------
// Triangle-box overlap (13-axis SAT). All axes handled with one projection
// helper: box faces (x,y,z), triangle normal, and edge x axis (9 of them).
// ---------------------------------------------------------------------------
bool triBoxOverlap(Vec3 boxCenter, Vec3 boxHalf, Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 v0 = a - boxCenter;
  const Vec3 v1 = b - boxCenter;
  const Vec3 v2 = c - boxCenter;

  const auto separates = [&](Vec3 axis) -> bool {
    if (std::abs(axis.x) < kEps && std::abs(axis.y) < kEps && std::abs(axis.z) < kEps) {
      return false;  // degenerate axis can't separate
    }
    const double p0 = dot(axis, v0);
    const double p1 = dot(axis, v1);
    const double p2 = dot(axis, v2);
    const double mn = std::min({p0, p1, p2});
    const double mx = std::max({p0, p1, p2});
    const double r = boxHalf.x * std::abs(axis.x) + boxHalf.y * std::abs(axis.y) +
                     boxHalf.z * std::abs(axis.z);
    return mn > r || mx < -r;
  };

  // 3 box-face normals.
  if (separates({1, 0, 0}) || separates({0, 1, 0}) || separates({0, 0, 1})) {
    return false;
  }
  const Vec3 e0 = v1 - v0;
  const Vec3 e1 = v2 - v1;
  const Vec3 e2 = v0 - v2;
  // Triangle face normal.
  if (separates(cross(e0, e1))) {
    return false;
  }
  // 9 edge x unit-axis cross products.
  const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (const Vec3& e : {e0, e1, e2}) {
    for (const Vec3& u : axes) {
      if (separates(cross(e, u))) {
        return false;
      }
    }
  }
  return true;
}

namespace {
// Möller–Trumbore; reports a forward (t > eps) hit with the semi-infinite ray.
bool rayHitsTriangle(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 e1 = b - a;
  const Vec3 e2 = c - a;
  const Vec3 p = cross(d, e2);
  const double det = dot(e1, p);
  if (std::abs(det) < kEps) {
    return false;
  }
  const double inv = 1.0 / det;
  const Vec3 tv = o - a;
  const double u = dot(tv, p) * inv;
  if (u < 0.0 || u > 1.0) {
    return false;
  }
  const Vec3 q = cross(tv, e1);
  const double v = dot(d, q) * inv;
  if (v < 0.0 || u + v > 1.0) {
    return false;
  }
  const double t = dot(e2, q) * inv;
  return t > kEps;
}

bool rayHitsAabb(Vec3 o, Vec3 inv, Vec3 lo, Vec3 hi) {
  double tmin = 0.0;
  double tmax = std::numeric_limits<double>::infinity();
  for (int ax = 0; ax < 3; ++ax) {
    const double oc = comp(o, ax);
    const double il = comp(inv, ax);
    double t1 = (comp(lo, ax) - oc) * il;
    double t2 = (comp(hi, ax) - oc) * il;
    if (t1 > t2) {
      std::swap(t1, t2);
    }
    tmin = std::max(tmin, t1);
    tmax = std::min(tmax, t2);
    if (tmax < tmin) {
      return false;
    }
  }
  return true;
}
}  // namespace

// ---------------------------------------------------------------------------
// BVH construction (median split on the longest centroid axis).
// ---------------------------------------------------------------------------
SolidGeometry::SolidGeometry(const TriMesh& mesh) : mesh_(mesh) {
  const std::size_t n = mesh_.tris.size();
  v0_.resize(n);
  v1_.resize(n);
  v2_.resize(n);
  triLo_.resize(n);
  triHi_.resize(n);
  tri_.resize(n);
  std::vector<Vec3> centroid(n);
  for (std::size_t i = 0; i < n; ++i) {
    v0_[i] = mesh_.vertices[mesh_.tris[i][0]];
    v1_[i] = mesh_.vertices[mesh_.tris[i][1]];
    v2_[i] = mesh_.vertices[mesh_.tris[i][2]];
    triLo_[i] = vmin(vmin(v0_[i], v1_[i]), v2_[i]);
    triHi_[i] = vmax(vmax(v0_[i], v1_[i]), v2_[i]);
    centroid[i] = (v0_[i] + v1_[i] + v2_[i]) * (1.0 / 3.0);
    tri_[i] = static_cast<uint32_t>(i);
  }
  if (n > 0) {
    nodes_.reserve(2 * n);
    buildRange(0, n, centroid);
  }
}

int SolidGeometry::buildRange(std::size_t begin, std::size_t end, std::vector<Vec3>& cen) {
  Node node;
  Vec3 lo{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::infinity()};
  Vec3 hi = lo * -1.0;
  for (std::size_t i = begin; i < end; ++i) {
    lo = vmin(lo, triLo_[tri_[i]]);
    hi = vmax(hi, triHi_[tri_[i]]);
  }
  node.lo = lo;
  node.hi = hi;

  const std::size_t count = end - begin;
  if (count <= kLeafSize) {
    node.start = static_cast<uint32_t>(begin);
    node.count = static_cast<uint32_t>(count);
    nodes_.push_back(node);
    return static_cast<int>(nodes_.size()) - 1;
  }

  const Vec3 ext = hi - lo;
  const int axis = (ext.x >= ext.y && ext.x >= ext.z) ? 0 : (ext.y >= ext.z ? 1 : 2);
  const std::size_t mid = begin + count / 2;
  std::nth_element(tri_.begin() + static_cast<long>(begin),
                   tri_.begin() + static_cast<long>(mid),
                   tri_.begin() + static_cast<long>(end),
                   [&](uint32_t x, uint32_t y) {
                     return comp(cen[x], axis) < comp(cen[y], axis);
                   });

  const int self = static_cast<int>(nodes_.size());
  nodes_.push_back(node);  // reserve slot; children may reallocate vector, so
                           // re-access via index, not reference.
  const int l = buildRange(begin, mid, cen);
  const int r = buildRange(mid, end, cen);
  nodes_[static_cast<std::size_t>(self)].left = l;
  nodes_[static_cast<std::size_t>(self)].right = r;
  nodes_[static_cast<std::size_t>(self)].count = 0;
  return self;
}

bool SolidGeometry::intersectsBox(Vec3 lo, Vec3 hi) const {
  if (nodes_.empty()) {
    return false;
  }
  const Vec3 center = (lo + hi) * 0.5;
  const Vec3 half = (hi - lo) * 0.5;
  int stack[64];
  int sp = 0;
  stack[sp++] = 0;
  while (sp > 0) {
    const Node& nd = nodes_[static_cast<std::size_t>(stack[--sp])];
    // AABB-AABB overlap of query box vs node box.
    if (lo.x > nd.hi.x || hi.x < nd.lo.x || lo.y > nd.hi.y || hi.y < nd.lo.y ||
        lo.z > nd.hi.z || hi.z < nd.lo.z) {
      continue;
    }
    if (nd.left < 0) {  // leaf
      for (uint32_t k = 0; k < nd.count; ++k) {
        const uint32_t t = tri_[nd.start + k];
        if (triBoxOverlap(center, half, v0_[t], v1_[t], v2_[t])) {
          return true;
        }
      }
    } else {
      stack[sp++] = nd.left;
      stack[sp++] = nd.right;
    }
  }
  return false;
}

int SolidGeometry::countRayHits(Vec3 o, Vec3 d) const {
  const Vec3 inv{1.0 / d.x, 1.0 / d.y, 1.0 / d.z};
  int hits = 0;
  int stack[64];
  int sp = 0;
  stack[sp++] = 0;
  while (sp > 0) {
    const Node& nd = nodes_[static_cast<std::size_t>(stack[--sp])];
    if (!rayHitsAabb(o, inv, nd.lo, nd.hi)) {
      continue;
    }
    if (nd.left < 0) {
      for (uint32_t k = 0; k < nd.count; ++k) {
        const uint32_t t = tri_[nd.start + k];
        if (rayHitsTriangle(o, d, v0_[t], v1_[t], v2_[t])) {
          ++hits;
        }
      }
    } else {
      stack[sp++] = nd.left;
      stack[sp++] = nd.right;
    }
  }
  return hits;
}

bool SolidGeometry::pointInside(Vec3 p) const {
  if (nodes_.empty()) {
    return false;
  }
  // Three generic, non-axis-aligned directions; majority parity vote.
  static const Vec3 dirs[3] = {normalized({0.4357, 0.8112, 0.3899}),
                               normalized({-0.7245, 0.2533, 0.6411}),
                               normalized({0.1934, -0.5772, 0.7933})};
  int insideVotes = 0;
  for (const Vec3& d : dirs) {
    if (countRayHits(p, d) % 2 == 1) {
      ++insideVotes;
    }
  }
  return insideVotes >= 2;
}

}  // namespace nabla::geometry
