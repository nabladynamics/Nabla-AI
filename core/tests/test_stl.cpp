#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nabla/geometry/stl_reader.hpp"
#include "nabla/geometry/trimesh.hpp"

using namespace nabla::geometry;

namespace {
std::string tmp(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}
}  // namespace

TEST_CASE("binary STL of a cube reads, welds to 8 vertices, is watertight", "[stl]") {
  const TriMesh box = makeBox({0, 0, 0}, {1, 1, 1});
  const std::string path = tmp("nabla_cube_bin.stl");
  writeBinaryStl(box, path);

  const StlReadResult r = readStl(path);
  REQUIRE(r.format == StlFormat::Binary);
  REQUIRE(r.report.rawTriangles == 12);
  REQUIRE(r.report.rawVertices == 36);
  REQUIRE(r.report.triangles == 12);
  REQUIRE(r.report.vertices == 8);          // 36 welded down to 8 corners
  REQUIRE(r.report.weldedVertices == 36 - 8);
  REQUIRE(r.report.removedDegenerate == 0);
  REQUIRE(r.report.watertight);
  REQUIRE(r.report.boundaryEdges == 0);
  REQUIRE(r.report.nonManifoldEdges == 0);
  REQUIRE(r.report.characteristicLength == Catch::Approx(std::sqrt(3.0)));
  std::filesystem::remove(path);
}

TEST_CASE("ASCII STL reads equivalently to binary", "[stl]") {
  const TriMesh box = makeBox({-1, -1, -1}, {1, 1, 1});
  const std::string path = tmp("nabla_cube_ascii.stl");
  writeAsciiStl(box, path);

  const StlReadResult r = readStl(path);
  REQUIRE(r.format == StlFormat::Ascii);
  REQUIRE(r.report.triangles == 12);
  REQUIRE(r.report.vertices == 8);
  REQUIRE(r.report.watertight);
  std::filesystem::remove(path);
}

TEST_CASE("a non-watertight STL is reported, not silently fixed", "[stl][broken]") {
  TriMesh box = makeBox({0, 0, 0}, {1, 1, 1});
  box.tris.pop_back();  // punch a hole: leaves an open boundary
  const std::string path = tmp("nabla_open.stl");
  writeAsciiStl(box, path);

  const StlReadResult r = readStl(path);  // must NOT throw
  REQUIRE(r.report.triangles == 11);
  REQUIRE_FALSE(r.report.watertight);
  REQUIRE(r.report.boundaryEdges > 0);
  std::filesystem::remove(path);
}

TEST_CASE("degenerate triangles are removed during cleaning", "[stl][broken]") {
  TriMesh box = makeBox({0, 0, 0}, {1, 1, 1});
  box.vertices.push_back({0, 0, 0});  // duplicate of corner 0
  const auto d = static_cast<uint32_t>(box.vertices.size() - 1);
  box.tris.push_back({d, d, d});      // zero-area, repeated-index triangle
  const std::string path = tmp("nabla_degen.stl");
  writeAsciiStl(box, path);

  const StlReadResult r = readStl(path);
  REQUIRE(r.report.removedDegenerate == 1);
  REQUIRE(r.report.triangles == 12);  // the cube survives intact
  REQUIRE(r.report.watertight);
  std::filesystem::remove(path);
}

TEST_CASE("garbage and empty files throw rather than crash", "[stl][broken]") {
  const std::string garbage = tmp("nabla_garbage.stl");
  {
    std::ofstream f(garbage);
    f << "this is definitely not an STL file\n";
  }
  REQUIRE_THROWS_AS(readStl(garbage), std::runtime_error);
  std::filesystem::remove(garbage);

  const std::string empty = tmp("nabla_empty.stl");
  { std::ofstream f(empty); }
  REQUIRE_THROWS_AS(readStl(empty), std::runtime_error);
  std::filesystem::remove(empty);

  REQUIRE_THROWS_AS(readStl(tmp("does_not_exist_12345.stl")), std::runtime_error);
}
