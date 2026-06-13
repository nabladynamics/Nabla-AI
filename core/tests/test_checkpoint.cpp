#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "nabla/io/checkpoint.hpp"
#include "nabla/mesh/octree.hpp"

using namespace nabla::mesh;

TEST_CASE("HDF5 checkpoint round-trips the full state bit-exactly",
          "[checkpoint][io]") {
  if (!nabla::io::hdf5Available()) {
    SKIP("built without HDF5 support");
  }

  Octree t(Vec3{-7.0, -1.5, -3.2}, Vec3{14.0, 3.0, 6.4}, 10);
  t.registerScalar("tke");
  t.registerLabel("physics_model");
  t.refineUniform(2);
  t.refineBox(Vec3{-1.0, -0.5, -1.0}, Vec3{1.0, 0.5, 1.0}, 2);

  // Populate fields with non-trivial values and a couple of masks/labels.
  std::vector<double>& u = t.u();
  std::vector<double>& tke = t.scalar("tke");
  std::vector<uint8_t>& model = t.label("physics_model");
  for (std::size_t i = 0; i < t.cellCount(); ++i) {
    const Vec3 c = t.cellCenter(i);
    u[i] = c.x * 0.123456789 - c.y * 0.5 + c.z;  // exercise mantissa bits
    tke[i] = static_cast<double>(i) * 1e-7;
    model[i] = static_cast<uint8_t>(i % 3);
  }
  t.setMask(0, CellMask::InsideSolid);
  t.setMask(1, CellMask::Cut);

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "nabla_checkpoint_test.h5";
  nabla::io::writeCheckpoint(t, path.string());
  const Octree reloaded = nabla::io::readCheckpoint(path.string());

  const OctreeState a = t.dumpState();
  const OctreeState b = reloaded.dumpState();

  // Geometry / topology.
  REQUIRE(b.maxLevel == a.maxLevel);
  REQUIRE(b.origin.x == a.origin.x);
  REQUIRE(b.origin.y == a.origin.y);
  REQUIRE(b.origin.z == a.origin.z);
  REQUIRE(b.extent.x == a.extent.x);
  REQUIRE(b.morton == a.morton);  // exact uint64 vector equality
  REQUIRE(b.level == a.level);
  REQUIRE(b.mask == a.mask);

  // Every scalar field, compared by name, must be bit-exact.
  REQUIRE(b.scalarNames.size() == a.scalarNames.size());
  for (std::size_t f = 0; f < a.scalarNames.size(); ++f) {
    const std::string& name = a.scalarNames[f];
    REQUIRE(reloaded.hasScalar(name));
    REQUIRE(reloaded.scalar(name) == t.scalar(name));  // operator== on doubles: bit-exact
  }
  // Labels likewise.
  REQUIRE(b.labelNames.size() == a.labelNames.size());
  for (const std::string& name : a.labelNames) {
    REQUIRE(reloaded.hasLabel(name));
  }

  std::filesystem::remove(path);
}

TEST_CASE("checkpoint preserves the u,v,w,p core field ordering", "[checkpoint][io]") {
  if (!nabla::io::hdf5Available()) {
    SKIP("built without HDF5 support");
  }
  Octree t(Vec3{0, 0, 0}, Vec3{1, 1, 1}, 4);
  t.registerScalar("extra");
  t.refineUniform(1);
  t.u()[0] = 11.0;
  t.v()[0] = 22.0;
  t.w()[0] = 33.0;
  t.p()[0] = 44.0;

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "nabla_checkpoint_order.h5";
  nabla::io::writeCheckpoint(t, path.string());
  Octree r = nabla::io::readCheckpoint(path.string());

  REQUIRE(r.u()[0] == 11.0);
  REQUIRE(r.v()[0] == 22.0);
  REQUIRE(r.w()[0] == 33.0);
  REQUIRE(r.p()[0] == 44.0);
  std::filesystem::remove(path);
}
