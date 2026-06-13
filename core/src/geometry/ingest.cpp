#include "nabla/geometry/ingest.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "nabla/geometry/classify.hpp"
#include "nabla/geometry/features.hpp"
#include "nabla/geometry/geometry_report.hpp"
#include "nabla/geometry/solid_geometry.hpp"
#include "nabla/geometry/stl_reader.hpp"
#include "nabla/geometry/trimesh.hpp"
#include "nabla/io/vtu_writer.hpp"
#include "nabla/mesh/octree.hpp"

namespace nabla::geometry {
namespace {

MeshStats collectStats(const mesh::Octree& tree) {
  MeshStats s;
  s.cells = tree.cellCount();
  s.minLevel = tree.maxLevel();
  s.maxLevel = 0;
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    switch (tree.mask(i)) {
      case mesh::CellMask::InsideSolid: ++s.insideSolid; break;
      case mesh::CellMask::Cut: ++s.cut; break;
      case mesh::CellMask::Fluid: ++s.fluid; break;
    }
    s.minLevel = std::min(s.minLevel, tree.level(i));
    s.maxLevel = std::max(s.maxLevel, tree.level(i));
  }
  s.balanced = tree.isBalanced();
  return s;
}

}  // namespace

IngestSummary runIngest(const std::string& stlPath, const IngestOptions& options) {
  StlReadResult stl = readStl(stlPath);  // read + clean (throws if unparseable)
  TriMesh& mesh = stl.mesh;

  DomainInfo domain;
  domain.caseName = options.caseName;

  // ---- domain construction + object placement ----------------------------
  if (options.caseName == "wall-mounted-cube") {
    const Vec3 size = stl.report.bboxHi - stl.report.bboxLo;
    const double h = size.y;  // cube height drives the channel dimensions
    if (h <= 0.0) {
      throw std::runtime_error("ingest: degenerate cube height (empty STL?)");
    }
    domain.cubeHeight = h;
    domain.origin = {0.0, 0.0, 0.0};
    domain.extent = {14.0 * h, 3.0 * h, 6.4 * h};

    // Place the cube: front face 3h from the inlet, sitting on the floor,
    // centered in the span.
    const Vec3 targetMin{3.0 * h, 0.0, 3.2 * h - 0.5 * size.z};
    mesh.translate(targetMin - stl.report.bboxLo);
  } else {
    // Generic: a padded box around the object bbox.
    const Vec3 size = stl.report.bboxHi - stl.report.bboxLo;
    const double pad = std::max({size.x, size.y, size.z});
    domain.cubeHeight = std::min({size.x, size.y, size.z});
    domain.origin = stl.report.bboxLo - Vec3{pad, pad, pad};
    domain.extent = size + Vec3{2 * pad, 2 * pad, 2 * pad};
  }

  // ---- features + solid geometry (in domain coordinates) -----------------
  const FeatureSet features = detectFeatures(mesh, options.sharpAngleDeg);
  const SolidGeometry solid(mesh);

  // ---- build + refine the octree -----------------------------------------
  mesh::Octree tree(domain.origin, domain.extent, options.maxLevel);
  tree.refineUniform(options.baseLevel);

  const double featureScale = domain.cubeHeight > 0.0 ? domain.cubeHeight
                                                      : stl.report.characteristicLength;
  const double surfaceTarget = featureScale / std::max(1.0, options.resolution);
  const double edgeTarget =
      surfaceTarget / static_cast<double>(1 << std::max(0, options.edgeExtraLevels));

  geometryRefine(tree, solid, features, surfaceTarget, edgeTarget);
  classifyOntoOctree(tree, solid, features);

  // ---- outputs ------------------------------------------------------------
  MeshStats stats = collectStats(tree);
  stats.surfaceTarget = surfaceTarget;
  stats.edgeTarget = edgeTarget;

  const std::string vtuPath =
      options.vtuPath.empty() ? "ingest.vtu" : options.vtuPath;
  const std::string reportPath =
      options.reportPath.empty() ? "ingest.geometry.json" : options.reportPath;

  io::writeVtu(tree, vtuPath);
  writeGeometryReport(reportPath, stlPath, stl, features, domain, stats);

  IngestSummary summary;
  summary.cells = stats.cells;
  summary.insideSolid = stats.insideSolid;
  summary.cut = stats.cut;
  summary.fluid = stats.fluid;
  summary.minLevel = stats.minLevel;
  summary.maxLevel = stats.maxLevel;
  summary.balanced = stats.balanced;
  summary.watertight = stl.report.watertight;
  summary.sharpEdges = features.sharpEdges.size();
  summary.corners = features.corners.size();
  summary.cubeHeight = domain.cubeHeight;
  summary.vtuPath = vtuPath;
  summary.reportPath = reportPath;
  return summary;
}

}  // namespace nabla::geometry
