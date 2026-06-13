#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "nabla/geometry/classify.hpp"
#include "nabla/geometry/features.hpp"
#include "nabla/geometry/ingest.hpp"
#include "nabla/geometry/solid_geometry.hpp"
#include "nabla/geometry/stl_reader.hpp"
#include "nabla/geometry/trimesh.hpp"
#include "nabla/mesh/octree.hpp"

using namespace nabla::geometry;

namespace {
std::string tmp(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}
}  // namespace

TEST_CASE("triangle-box overlap separates inside, crossing, and outside boxes",
          "[geometry][sat]") {
  const Vec3 a{0, 0, 0}, b{1, 0, 0}, c{0, 1, 0};  // triangle in z=0
  REQUIRE(triBoxOverlap({0.25, 0.25, 0.0}, {0.1, 0.1, 0.1}, a, b, c));   // crossing
  REQUIRE_FALSE(triBoxOverlap({0.25, 0.25, 1.0}, {0.1, 0.1, 0.1}, a, b, c));  // above
  REQUIRE_FALSE(triBoxOverlap({5.0, 5.0, 0.0}, {0.1, 0.1, 0.1}, a, b, c));    // far away
}

TEST_CASE("point-in-solid is correct for a cube", "[geometry][raycast]") {
  const SolidGeometry solid(makeBox({0, 0, 0}, {1, 1, 1}));
  REQUIRE(solid.pointInside({0.5, 0.5, 0.5}));
  REQUIRE_FALSE(solid.pointInside({1.5, 0.5, 0.5}));
  REQUIRE_FALSE(solid.pointInside({-0.5, 0.5, 0.5}));
  REQUIRE(solid.pointInside({0.1, 0.9, 0.2}));
}

TEST_CASE("uniform-depth classification yields the exact solid cell count",
          "[geometry][classify]") {
  // Domain [0,1]^3 at depth 3 => 8^3 cells of size 0.125. Solid cube [0.3,0.7]^3
  // is deliberately not cell-aligned, so the inside count is unambiguous:
  // interior cells are exactly indices {3,4}^3 = 8.
  const TriMesh solidMesh = makeBox({0.3, 0.3, 0.3}, {0.7, 0.7, 0.7});
  const SolidGeometry solid(solidMesh);
  const FeatureSet fs = detectFeatures(solidMesh, 30.0);

  nabla::mesh::Octree tree(nabla::mesh::Vec3{0, 0, 0}, nabla::mesh::Vec3{1, 1, 1}, 6);
  tree.refineUniform(3);
  REQUIRE(tree.cellCount() == 512);

  classifyOntoOctree(tree, solid, fs);

  std::size_t inside = 0, cut = 0, fluid = 0;
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    switch (tree.mask(i)) {
      case nabla::mesh::CellMask::InsideSolid: ++inside; break;
      case nabla::mesh::CellMask::Cut: ++cut; break;
      case nabla::mesh::CellMask::Fluid: ++fluid; break;
    }
  }
  REQUIRE(inside == 8);
  REQUIRE(cut == 56);
  REQUIRE(fluid == 512 - 64);

  // CUT cells carry the no-slip wall flag; nothing else does.
  const std::vector<uint8_t>& wall = tree.label(kWallNoSlipLabel);
  std::size_t wallCount = 0;
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    wallCount += wall[i];
  }
  REQUIRE(wallCount == cut);
}

TEST_CASE("sharp edges receive extra refinement (separation points not rounded)",
          "[geometry][refine]") {
  // Replicate the wall-mounted-cube placement (h = 1).
  const TriMesh cube = makeBox({3.0, 0.0, 2.7}, {4.0, 1.0, 3.7});
  const SolidGeometry solid(cube);
  const FeatureSet fs = detectFeatures(cube, 30.0);

  nabla::mesh::Octree tree(nabla::mesh::Vec3{0, 0, 0}, nabla::mesh::Vec3{14, 3, 6.4}, 10);
  tree.refineUniform(3);
  const double surfaceTarget = 1.0 / 4.0;   // h / resolution(4)
  const double edgeTarget = surfaceTarget / 4.0;  // edgeExtraLevels = 2
  geometryRefine(tree, solid, fs, surfaceTarget, edgeTarget);
  classifyOntoOctree(tree, solid, fs);

  const std::vector<double>& nearEdge = tree.scalar(kNearSharpEdgeScalar);
  int maxEdgeLevel = 0;
  int maxCutLevel = 0;
  int minCutLevel = tree.maxLevel();
  std::size_t edgeCells = 0;
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    if (tree.mask(i) != nabla::mesh::CellMask::Cut) {
      continue;
    }
    const int lvl = tree.level(i);
    maxCutLevel = std::max(maxCutLevel, lvl);
    minCutLevel = std::min(minCutLevel, lvl);
    if (nearEdge[i] > 0.5) {
      maxEdgeLevel = std::max(maxEdgeLevel, lvl);
      ++edgeCells;
    }
  }
  REQUIRE(edgeCells > 0);
  // Edge bands are among the finest cells, and the flat surface away from edges
  // is coarser — i.e., the extra refinement is concentrated at the sharp edges.
  REQUIRE(maxEdgeLevel == maxCutLevel);
  REQUIRE(maxCutLevel - minCutLevel >= 1);
  REQUIRE(tree.isBalanced());
}

TEST_CASE("full ingest of a unit cube writes a balanced domain + report",
          "[geometry][ingest]") {
  const std::string stl = tmp("nabla_ingest_cube.stl");
  writeBinaryStl(makeBox({0, 0, 0}, {1, 1, 1}), stl);

  IngestOptions opt;
  opt.caseName = "wall-mounted-cube";
  opt.baseLevel = 3;
  opt.resolution = 4.0;
  opt.edgeExtraLevels = 2;
  opt.maxLevel = 9;
  opt.vtuPath = tmp("nabla_ingest_cube.vtu");
  opt.reportPath = tmp("nabla_ingest_cube.json");

  const IngestSummary s = runIngest(stl, opt);
  REQUIRE(s.watertight);
  REQUIRE(s.sharpEdges == 12);
  REQUIRE(s.corners == 8);
  REQUIRE(s.balanced);
  REQUIRE(s.cut > 0);
  REQUIRE(s.insideSolid > 0);
  REQUIRE(s.cubeHeight == 1.0);
  REQUIRE(s.maxLevel > opt.baseLevel);  // surface/edge refinement happened
  REQUIRE(std::filesystem::exists(opt.vtuPath));
  REQUIRE(std::filesystem::exists(opt.reportPath));

  std::filesystem::remove(stl);
  std::filesystem::remove(opt.vtuPath);
  std::filesystem::remove(opt.reportPath);
}
