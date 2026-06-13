#include "nabla/geometry/classify.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace nabla::geometry {
namespace {

// Cell box for a leaf.
void cellBox(const mesh::Octree& t, std::size_t i, Vec3& lo, Vec3& hi) {
  const Vec3 c = t.cellCenter(i);
  const Vec3 h = t.cellSize(i) * 0.5;
  lo = c - h;
  hi = c + h;
}

double maxDim(const mesh::Octree& t, std::size_t i) {
  const Vec3 s = t.cellSize(i);
  return std::max({s.x, s.y, s.z});
}

// Squared distance from point p to segment [a,b].
double distSqPointSegment(Vec3 p, Vec3 a, Vec3 b) {
  const Vec3 ab = b - a;
  const double len2 = dot(ab, ab);
  double tparam = len2 > 0.0 ? dot(p - a, ab) / len2 : 0.0;
  tparam = std::clamp(tparam, 0.0, 1.0);
  const Vec3 proj = a + ab * tparam;
  const Vec3 d = p - proj;
  return dot(d, d);
}

// Does segment [a,b] come within the AABB [lo,hi] (expanded test via center +
// half + a conservative sphere on the box)? We use a cheap-but-safe check: the
// segment overlaps the box if its closest point to the box center is within the
// box's bounding sphere AND a slab test passes. For robustness we use the
// standard "segment vs AABB" via clamping endpoints + center proximity.
bool segmentNearBox(Vec3 a, Vec3 b, Vec3 lo, Vec3 hi) {
  const Vec3 center = (lo + hi) * 0.5;
  const Vec3 half = (hi - lo) * 0.5;
  // Quick reject: segment AABB vs cell AABB.
  const Vec3 slo = vmin(a, b);
  const Vec3 shi = vmax(a, b);
  if (slo.x > hi.x || shi.x < lo.x || slo.y > hi.y || shi.y < lo.y || slo.z > hi.z ||
      shi.z < lo.z) {
    return false;
  }
  // Accept if either endpoint is inside, or the segment's closest point to the
  // cell center lies within the (slightly inflated) cell. This is conservative
  // — it never misses an edge that crosses the cell, which is what we need for
  // refinement (a few false positives near the cell only add safe refinement).
  const double r2 = dot(half, half) * 1.0001;
  return distSqPointSegment(center, a, b) <= r2;
}

}  // namespace

void classifyOntoOctree(mesh::Octree& tree, const SolidGeometry& solid,
                        const FeatureSet& features) {
  tree.registerLabel(kWallNoSlipLabel);
  tree.registerScalar(kNearSharpEdgeScalar);
  std::vector<uint8_t>& wall = tree.label(kWallNoSlipLabel);
  std::vector<double>& nearEdge = tree.scalar(kNearSharpEdgeScalar);

  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    Vec3 lo, hi;
    cellBox(tree, i, lo, hi);
    wall[i] = 0;
    nearEdge[i] = 0.0;
    if (solid.intersectsBox(lo, hi)) {
      tree.setMask(i, mesh::CellMask::Cut);
      wall[i] = 1;  // no-slip wall
      for (const SharpEdge& se : features.sharpEdges) {
        if (segmentNearBox(se.a, se.b, lo, hi)) {
          nearEdge[i] = 1.0;
          break;
        }
      }
    } else if (solid.pointInside(tree.cellCenter(i))) {
      tree.setMask(i, mesh::CellMask::InsideSolid);
    } else {
      tree.setMask(i, mesh::CellMask::Fluid);
    }
  }
}

int geometryRefine(mesh::Octree& tree, const SolidGeometry& solid,
                   const FeatureSet& features, double surfaceTarget, double edgeTarget) {
  int passes = 0;
  for (;;) {
    std::vector<uint64_t> toRefine;
    for (std::size_t i = 0; i < tree.cellCount(); ++i) {
      Vec3 lo, hi;
      cellBox(tree, i, lo, hi);
      if (!solid.intersectsBox(lo, hi)) {
        continue;  // only refine the surface band
      }
      if (tree.level(i) >= tree.maxLevel()) {
        continue;
      }
      bool nearEdge = false;
      for (const SharpEdge& se : features.sharpEdges) {
        if (segmentNearBox(se.a, se.b, lo, hi)) {
          nearEdge = true;
          break;
        }
      }
      const double target = nearEdge ? edgeTarget : surfaceTarget;
      if (maxDim(tree, i) > target) {
        toRefine.push_back(tree.morton(i));
      }
    }
    if (toRefine.empty()) {
      break;
    }
    for (const uint64_t key : toRefine) {
      if (tree.find(key) != mesh::Octree::npos) {
        tree.refineByMorton(key);
      }
    }
    ++passes;
    if (passes > 64) {
      break;  // safety guard
    }
  }
  return passes;
}

}  // namespace nabla::geometry
