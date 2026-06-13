#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nabla/flow/octree_poisson.hpp"
#include "nabla/mesh/octree.hpp"

using namespace nabla;
using nabla::flow::OctreePoisson;
using nabla::mesh::Octree;
using nabla::mesh::Vec3;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Manufactured pressure with zero normal gradient on the unit-box faces, so it
// is compatible with the closed-Neumann octree domain.
double manufactured(const Vec3& c) {
  return std::cos(kPi * c.x) * std::cos(kPi * c.y) * std::cos(kPi * c.z);
}

double meanOver(const std::vector<double>& a) {
  double s = 0.0;
  for (double v : a) {
    s += v;
  }
  return s / static_cast<double>(a.size());
}

// L2 difference of two fields compared up to an additive constant (the Neumann
// null space).
double l2UpToConstant(const std::vector<double>& a, const std::vector<double>& b) {
  const double ma = meanOver(a), mb = meanOver(b);
  double err = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double e = (a[i] - ma) - (b[i] - mb);
    err += e * e;
  }
  return std::sqrt(err / static_cast<double>(a.size()));
}
}  // namespace

TEST_CASE("octree Poisson recovers a manufactured solution on a uniform octree",
          "[flow][octree][poisson]") {
  Octree tree(Vec3{0, 0, 0}, Vec3{1, 1, 1}, 4);
  tree.refineUniform(3);  // 8^3 = 512 leaves, all Fluid

  std::vector<double> pExact(tree.cellCount());
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    pExact[i] = manufactured(tree.cellCenter(i));
  }
  std::vector<double> b;
  OctreePoisson::applyA(tree, pExact, b);  // b = A pExact (compatible by construction)

  OctreePoisson solver(1e-10, 5000);
  std::vector<double> x;
  const auto rep = solver.solve(tree, b, x);

  REQUIRE(rep.residual < 1e-8);
  REQUIRE(l2UpToConstant(x, pExact) < 1e-6);
}

TEST_CASE("octree Poisson recovers a manufactured solution across a 2:1 level jump",
          "[flow][octree][poisson]") {
  Octree tree(Vec3{0, 0, 0}, Vec3{1, 1, 1}, 5);
  tree.refineUniform(2);                                  // 4^3 = 64 leaves at level 2
  tree.refineBox(Vec3{0, 0, 0}, Vec3{0.5, 0.5, 0.5}, 1);  // refine one octant -> 2:1 interfaces

  REQUIRE(tree.isBalanced());
  // Confirm the mesh really is mixed-level (so the interface is exercised).
  int lo = 99, hi = 0;
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    lo = std::min(lo, tree.level(i));
    hi = std::max(hi, tree.level(i));
  }
  REQUIRE(hi > lo);

  std::vector<double> pExact(tree.cellCount());
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    pExact[i] = manufactured(tree.cellCenter(i));
  }
  std::vector<double> b;
  OctreePoisson::applyA(tree, pExact, b);

  OctreePoisson solver(1e-10, 5000);
  std::vector<double> x;
  const auto rep = solver.solve(tree, b, x);

  REQUIRE(rep.residual < 1e-8);
  // If the coarse-fine flux matching were inconsistent, A pExact would not be a
  // recoverable RHS and this would blow up.
  REQUIRE(l2UpToConstant(x, pExact) < 1e-6);
}

TEST_CASE("octree Poisson operator is conservative across level jumps (sum A p = 0)",
          "[flow][octree][poisson]") {
  Octree tree(Vec3{0, 0, 0}, Vec3{1, 1, 1}, 5);
  tree.refineUniform(2);
  tree.refineBox(Vec3{0, 0, 0}, Vec3{0.5, 0.5, 0.5}, 1);

  std::mt19937 rng(2024);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  std::vector<double> p(tree.cellCount());
  double maxAbs = 0.0;
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    p[i] = dist(rng);
  }
  std::vector<double> Ap;
  OctreePoisson::applyA(tree, p, Ap);
  double sum = 0.0;
  for (double v : Ap) {
    sum += v;
    maxAbs = std::max(maxAbs, std::abs(v));
  }
  // Every interior face flux appears twice with opposite sign; boundary faces
  // are skipped (Neumann). So the net must be ~0 regardless of p.
  REQUIRE(std::abs(sum) < 1e-9 * (1.0 + maxAbs));
}

TEST_CASE("octree Poisson operator is symmetric (SPD)", "[flow][octree][poisson]") {
  Octree tree(Vec3{0, 0, 0}, Vec3{1, 1, 1}, 5);
  tree.refineUniform(2);
  tree.refineBox(Vec3{0, 0, 0}, Vec3{0.5, 0.5, 0.5}, 1);

  std::mt19937 rng(7);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  std::vector<double> a(tree.cellCount()), bvec(tree.cellCount());
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    a[i] = dist(rng);
    bvec[i] = dist(rng);
  }
  std::vector<double> Aa, Ab;
  OctreePoisson::applyA(tree, a, Aa);
  OctreePoisson::applyA(tree, bvec, Ab);
  double aAb = 0.0, bAa = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    aAb += a[i] * Ab[i];
    bAa += bvec[i] * Aa[i];
  }
  REQUIRE(aAb == Catch::Approx(bAa).epsilon(1e-10));
}
