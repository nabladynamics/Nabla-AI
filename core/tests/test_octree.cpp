#include <cstddef>
#include <random>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nabla/mesh/octree.hpp"

using namespace nabla::mesh;

namespace {

Octree makeTree(int maxLevel = 10) {
  return Octree(Vec3{-7.0, -1.5, -3.2}, Vec3{14.0, 3.0, 6.4}, maxLevel);
}

double integralOf(const Octree& t, const std::vector<double>& field) {
  double sum = 0.0;
  for (std::size_t i = 0; i < t.cellCount(); ++i) {
    sum += field[i] * t.cellVolume(i);
  }
  return sum;
}

}  // namespace

TEST_CASE("root cell covers the whole domain", "[octree]") {
  Octree t = makeTree();
  REQUIRE(t.cellCount() == 1);
  const Vec3 c = t.cellCenter(0);
  REQUIRE(c.x == Catch::Approx(0.0));
  REQUIRE(c.y == Catch::Approx(0.0));
  REQUIRE(c.z == Catch::Approx(0.0));
  REQUIRE(t.cellVolume(0) == Catch::Approx(14.0 * 3.0 * 6.4));
}

TEST_CASE("uniform refinement keeps cell count and is balanced", "[octree]") {
  Octree t = makeTree();
  t.refineUniform(3);
  REQUIRE(t.cellCount() == 8u * 8u * 8u);  // 8^3
  REQUIRE(t.isBalanced());
  // Total volume is conserved under refinement.
  double vol = 0.0;
  for (std::size_t i = 0; i < t.cellCount(); ++i) {
    vol += t.cellVolume(i);
  }
  REQUIRE(vol == Catch::Approx(14.0 * 3.0 * 6.4));
}

TEST_CASE("refining one cell creates a 2:1 jump that balance resolves", "[octree]") {
  Octree t = makeTree();
  t.refineUniform(2);            // 4x4x4, all level 2
  const std::size_t target = t.find(t.morton(0));
  t.refine(target);             // a single level-2 -> level-3 split
  REQUIRE(t.isBalanced());
  // The coarse face neighbor of a fine child should see up to 4 finer neighbors.
}

TEST_CASE("2:1 balance holds after a random refine storm", "[octree][balance]") {
  Octree t = makeTree(9);
  t.refineUniform(2);
  std::mt19937_64 rng(12345);
  for (int step = 0; step < 250; ++step) {
    const auto n = t.cellCount();
    std::uniform_int_distribution<std::size_t> pick(0, n - 1);
    const std::size_t i = pick(rng);
    if (t.level(i) < t.maxLevel()) {
      t.refine(i);
    }
    if (step % 25 == 0) {
      REQUIRE(t.isBalanced());  // stays balanced throughout, not just at the end
    }
  }
  REQUIRE(t.isBalanced());
  REQUIRE(t.cellCount() > 64);
}

TEST_CASE("refine then coarsen restores the parent exactly", "[octree]") {
  Octree t = makeTree();
  t.registerScalar("phi");
  t.refineUniform(1);
  std::vector<double>& phi = t.scalar("phi");
  for (std::size_t i = 0; i < t.cellCount(); ++i) {
    phi[i] = 1.0 + static_cast<double>(i);
  }
  const std::size_t before = t.cellCount();
  const double valueBefore = t.scalar("phi")[3];
  const uint64_t key = t.morton(3);
  const int lvl = t.level(3);

  t.refine(t.find(key));
  REQUIRE(t.cellCount() == before + 7);  // -1 parent, +8 children

  // Coarsen the family back; injection->average is the identity here.
  REQUIRE(t.coarsen(key, lvl));
  REQUIRE(t.cellCount() == before);
  REQUIRE(t.scalar("phi")[t.find(key)] == Catch::Approx(valueBefore));
}

TEST_CASE("integrated field quantity is conserved under refine/coarsen storms",
          "[octree][conservation]") {
  Octree t = makeTree(8);
  t.registerScalar("phi");
  t.refineUniform(2);
  std::vector<double>& phi0 = t.scalar("phi");
  for (std::size_t i = 0; i < t.cellCount(); ++i) {
    const Vec3 c = t.cellCenter(i);
    phi0[i] = 2.0 + c.x - 0.5 * c.y + 0.25 * c.z * c.z;
  }
  const double integralBefore = integralOf(t, t.scalar("phi"));

  std::mt19937_64 rng(777);
  for (int step = 0; step < 400; ++step) {
    const std::size_t n = t.cellCount();
    std::uniform_int_distribution<std::size_t> pick(0, n - 1);
    const std::size_t i = pick(rng);
    std::uniform_int_distribution<int> coin(0, 1);
    if (coin(rng) == 0) {
      if (t.level(i) < t.maxLevel()) {
        t.refine(i);
      }
    } else {
      if (t.level(i) > 0) {
        t.coarsenSiblings(i);  // may be refused (returns false) — that's fine
      }
    }
  }

  const double integralAfter = integralOf(t, t.scalar("phi"));
  REQUIRE(t.isBalanced());
  REQUIRE(integralAfter ==
          Catch::Approx(integralBefore).epsilon(1e-9).margin(1e-9));
}

TEST_CASE("face neighbor counts respect level jumps", "[octree][neighbors]") {
  Octree t = makeTree();
  t.refineUniform(2);  // 4x4x4
  // Pick an interior cell (away from all 6 boundaries) and refine it.
  std::size_t interior = Octree::npos;
  for (std::size_t i = 0; i < t.cellCount(); ++i) {
    bool hasAll = true;
    for (int d = 0; d < 6; ++d) {
      if (t.faceNeighbors(i, d).empty()) {
        hasAll = false;
        break;
      }
    }
    if (hasAll) {
      interior = i;
      break;
    }
  }
  REQUIRE(interior != Octree::npos);
  // Same-level interior cell: exactly one neighbor per face.
  for (int d = 0; d < 6; ++d) {
    REQUIRE(t.faceNeighbors(interior, d).size() == 1u);
  }

  // Remember the cell on the +x side, then refine the interior cell.
  const uint64_t eastKey = t.morton(t.faceNeighbors(interior, /*+x*/ 1).front());
  const uint64_t key = t.morton(interior);
  t.refine(t.find(key));

  // Looking back (-x) from the coarse east cell, we now see 4 finer neighbors.
  const std::size_t east = t.find(eastKey);
  REQUIRE(east != Octree::npos);
  REQUIRE(t.faceNeighbors(east, /*-x*/ 0).size() == 4u);
  REQUIRE(t.isBalanced());
}

TEST_CASE("gradient of a linear field is exact on a uniform mesh", "[octree][gradient]") {
  Octree t = makeTree();
  t.registerScalar("phi");
  t.refineUniform(3);
  std::vector<double>& phi = t.scalar("phi");
  const double a = 2.0, b = -3.0, c = 0.5;
  for (std::size_t i = 0; i < t.cellCount(); ++i) {
    const Vec3 ctr = t.cellCenter(i);
    phi[i] = a * ctr.x + b * ctr.y + c * ctr.z;
  }
  // Find a fully-interior cell.
  std::size_t interior = Octree::npos;
  for (std::size_t i = 0; i < t.cellCount(); ++i) {
    bool ok = true;
    for (int d = 0; d < 6; ++d) {
      if (t.faceNeighbors(i, d).size() != 1u) {
        ok = false;
        break;
      }
    }
    if (ok) {
      interior = i;
      break;
    }
  }
  REQUIRE(interior != Octree::npos);
  const Vec3 g = t.gradient(t.scalar("phi"), interior);
  REQUIRE(g.x == Catch::Approx(a));
  REQUIRE(g.y == Catch::Approx(b));
  REQUIRE(g.z == Catch::Approx(c));
}

TEST_CASE("solid mask is stored per cell and survives refinement", "[octree][mask]") {
  Octree t = makeTree();
  t.refineUniform(1);
  t.setMask(0, CellMask::InsideSolid);
  t.setMask(1, CellMask::Cut);
  const uint64_t key0 = t.morton(0);
  t.refine(t.find(key0));
  // Children inherit the parent's mask.
  for (std::size_t i = 0; i < t.cellCount(); ++i) {
    if (t.level(i) == 2) {
      REQUIRE(t.mask(i) == CellMask::InsideSolid);
    }
  }
}
