#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nabla/geometry/features.hpp"
#include "nabla/geometry/trimesh.hpp"

using namespace nabla::geometry;

TEST_CASE("sharp-edge detector finds all 12 cube edges and 8 corners", "[features]") {
  const TriMesh cube = makeBox({0, 0, 0}, {2, 2, 2});
  const FeatureSet fs = detectFeatures(cube, 30.0);

  REQUIRE(fs.sharpEdges.size() == 12);  // exactly the cube edges (face diagonals are flat)
  REQUIRE(fs.corners.size() == 8);

  for (const SharpEdge& e : fs.sharpEdges) {
    REQUIRE(e.dihedralDeg == Catch::Approx(90.0).margin(1e-6));
    REQUIRE(e.length == Catch::Approx(2.0));
  }
  REQUIRE(fs.smallestFeature == Catch::Approx(2.0));
}

TEST_CASE("a flat sheet has no sharp edges", "[features]") {
  // Two coplanar triangles forming a square in the z=0 plane.
  TriMesh sheet;
  sheet.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  sheet.tris = {{0, 1, 2}, {0, 2, 3}};
  const FeatureSet fs = detectFeatures(sheet, 30.0);
  REQUIRE(fs.sharpEdges.empty());
  REQUIRE(fs.corners.empty());
}

TEST_CASE("a high threshold suppresses the cube edges", "[features]") {
  const TriMesh cube = makeBox({0, 0, 0}, {1, 1, 1});
  const FeatureSet fs = detectFeatures(cube, 120.0);  // > 90 deg cube dihedral
  REQUIRE(fs.sharpEdges.empty());
}
