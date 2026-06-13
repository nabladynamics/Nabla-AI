#include "nabla/geometry/features.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace nabla::geometry {
namespace {

struct EKey {
  uint32_t a, b;
  bool operator==(const EKey& o) const { return a == o.a && b == o.b; }
};
struct EKeyHash {
  std::size_t operator()(const EKey& k) const {
    return (static_cast<std::size_t>(k.a) << 32) ^ k.b;
  }
};

EKey edgeKey(uint32_t i, uint32_t j) { return {std::min(i, j), std::max(i, j)}; }

constexpr double kPi = 3.14159265358979323846;

}  // namespace

FeatureSet detectFeatures(const TriMesh& mesh, double sharpAngleDeg) {
  FeatureSet fs;
  fs.sharpAngleDeg = sharpAngleDeg;
  const std::size_t nf = mesh.tris.size();

  // Precompute face normals.
  std::vector<Vec3> normal(nf);
  for (std::size_t f = 0; f < nf; ++f) {
    normal[f] = mesh.faceNormal(f);
  }

  // Edge -> incident faces.
  struct EdgeFaces {
    uint32_t va = 0, vb = 0;
    int f0 = -1, f1 = -1;
  };
  std::unordered_map<EKey, EdgeFaces, EKeyHash> edges;
  edges.reserve(nf * 3);
  const auto addEdge = [&](uint32_t i, uint32_t j, std::size_t f) {
    auto& e = edges[edgeKey(i, j)];
    e.va = std::min(i, j);
    e.vb = std::max(i, j);
    if (e.f0 < 0) {
      e.f0 = static_cast<int>(f);
    } else if (e.f1 < 0) {
      e.f1 = static_cast<int>(f);
    }
  };
  for (std::size_t f = 0; f < nf; ++f) {
    addEdge(mesh.tris[f][0], mesh.tris[f][1], f);
    addEdge(mesh.tris[f][1], mesh.tris[f][2], f);
    addEdge(mesh.tris[f][2], mesh.tris[f][0], f);
  }

  // Per-edge dihedral; collect sharp edges; accumulate per-face curvature.
  std::vector<double> faceAngleSum(nf, 0.0);
  std::vector<double> facePerimeter(nf, 0.0);
  std::unordered_map<uint32_t, int> sharpIncidence;  // vertex -> sharp-edge count

  const double cosThreshold = std::cos(sharpAngleDeg * kPi / 180.0);

  for (const auto& [key, e] : edges) {
    const double len = norm(mesh.vertices[e.va] - mesh.vertices[e.vb]);
    if (e.f0 >= 0 && e.f1 >= 0) {
      const double c = std::clamp(dot(normal[static_cast<std::size_t>(e.f0)],
                                      normal[static_cast<std::size_t>(e.f1)]),
                                  -1.0, 1.0);
      const double angle = std::acos(c);          // 0 = coplanar
      const double angleDeg = angle * 180.0 / kPi;
      faceAngleSum[static_cast<std::size_t>(e.f0)] += angle;
      faceAngleSum[static_cast<std::size_t>(e.f1)] += angle;
      facePerimeter[static_cast<std::size_t>(e.f0)] += len;
      facePerimeter[static_cast<std::size_t>(e.f1)] += len;
      if (c < cosThreshold) {  // dihedral > threshold => sharp
        SharpEdge se;
        se.a = mesh.vertices[e.va];
        se.b = mesh.vertices[e.vb];
        se.dihedralDeg = angleDeg;
        se.length = len;
        fs.sharpEdges.push_back(se);
        ++sharpIncidence[e.va];
        ++sharpIncidence[e.vb];
      }
    }
  }

  // Corners: vertices where 3+ sharp edges meet.
  for (const auto& [v, cnt] : sharpIncidence) {
    if (cnt >= 3) {
      fs.corners.push_back(mesh.vertices[v]);
    }
  }

  // Per-face curvature estimate ~ (sum dihedral) / perimeter  [1/length].
  fs.faceCurvature.resize(nf, 0.0);
  double cmin = std::numeric_limits<double>::infinity();
  double cmax = 0.0;
  double csum = 0.0;
  for (std::size_t f = 0; f < nf; ++f) {
    const double curv = facePerimeter[f] > 0.0 ? faceAngleSum[f] / facePerimeter[f] : 0.0;
    fs.faceCurvature[f] = curv;
    cmin = std::min(cmin, curv);
    cmax = std::max(cmax, curv);
    csum += curv;
  }
  fs.curvatureMin = nf ? cmin : 0.0;
  fs.curvatureMax = cmax;
  fs.curvatureMean = nf ? csum / static_cast<double>(nf) : 0.0;

  // Sharp-edge length statistics + smallest feature to resolve.
  double mn = std::numeric_limits<double>::infinity();
  double mx = 0.0;
  double total = 0.0;
  for (const SharpEdge& se : fs.sharpEdges) {
    mn = std::min(mn, se.length);
    mx = std::max(mx, se.length);
    total += se.length;
  }
  if (!fs.sharpEdges.empty()) {
    fs.minSharpEdgeLength = mn;
    fs.maxSharpEdgeLength = mx;
    fs.totalSharpEdgeLength = total;
    fs.smallestFeature = mn;
  } else {
    // Fall back to the shortest triangle edge.
    double minEdge = std::numeric_limits<double>::infinity();
    for (const auto& [key, e] : edges) {
      minEdge = std::min(minEdge, norm(mesh.vertices[e.va] - mesh.vertices[e.vb]));
    }
    fs.smallestFeature = std::isfinite(minEdge) ? minEdge : 0.0;
  }

  return fs;
}

}  // namespace nabla::geometry
