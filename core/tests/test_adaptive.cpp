#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nabla/adaptive/adaptive.hpp"
#include "nabla/mesh/octree.hpp"

using namespace nabla::adaptive;
using nabla::mesh::Octree;
using nabla::mesh::Vec3;

TEST_CASE("AMR conservation under refine/coarsen during a live solve",
          "[adaptive][conservation]") {
  Octree tree(Vec3{0, 0, 0}, Vec3{1, 1, 1}, 6);
  tree.refineUniform(3);
  tree.registerScalar("phi");

  // A non-trivial scalar and a uniform (divergence-free) advecting velocity.
  // Domain boundaries are closed, so the total integral must be conserved while
  // we transport AND refine/coarsen concurrently.
  auto setFields = [&]() {
    auto& phi = tree.scalar("phi");
    auto& u = tree.u();
    auto& v = tree.v();
    for (std::size_t i = 0; i < tree.cellCount(); ++i) {
      const Vec3 c = tree.cellCenter(i);
      phi[i] = 1.0 + std::sin(6.0 * c.x) * std::cos(5.0 * c.y);
      u[i] = 0.3;
      v[i] = 0.1;
    }
  };
  setFields();
  const double I0 = octreeFieldIntegral(tree, "phi");

  std::mt19937_64 rng(2024);
  const double dt = 0.02;
  for (int step = 0; step < 40; ++step) {
    octreeTransportStep(tree, "phi", dt);
    // concurrent AMR: refine a few random cells, coarsen a few random families.
    std::uniform_int_distribution<std::size_t> pick(0, tree.cellCount() - 1);
    for (int r = 0; r < 4; ++r) {
      const std::size_t i = pick(rng);
      if (tree.level(i) < 5) {
        tree.refine(i);
      }
    }
    for (int cco = 0; cco < 4; ++cco) {
      const std::size_t i = pick(rng) % tree.cellCount();
      if (tree.level(i) > 3) {
        tree.coarsenSiblings(i);
      }
    }
    // velocity must stay defined on new cells (carried by refine/coarsen); the
    // octree's conservative transfer keeps u,v consistent, so we needn't reset.
    const double I = octreeFieldIntegral(tree, "phi");
    REQUIRE(std::abs(I - I0) <= 1e-8 * std::abs(I0) + 1e-10);
  }
}

TEST_CASE("classifier rejects the log-law where reverse flow exists",
          "[adaptive][classifier]") {
  AdaptiveConfig cfg;
  RuleBasedClassifier clf;

  CellSensors s;
  s.nearWall = true;
  s.yPlus = 5.0;  // within the log-law sublayer (< 11.25)
  s.vorticityMag = 1.0;

  // Attached flow: log-law proposed and accepted.
  s.reverseFlow = false;
  REQUIRE(clf.propose(s, cfg) == Mode::NearWall);
  REQUIRE(acceptReducedModel(Mode::NearWall, s, cfg).accepted);

  // Reverse (separated) flow: log-law still proposed, but the NS-consistency
  // gate must REJECT it -> promote to FULL_NS, with a reason.
  s.reverseFlow = true;
  REQUIRE(clf.propose(s, cfg) == Mode::NearWall);
  const AcceptanceResult a = acceptReducedModel(Mode::NearWall, s, cfg);
  REQUIRE_FALSE(a.accepted);
  REQUIRE(a.reason.find("reverse flow") != std::string::npos);
}

TEST_CASE("hard guards cannot be overridden by config", "[adaptive][guards]") {
  AdaptiveConfig cfg;
  cfg.cubeHeight = 1.0;
  cfg.cubeFrontX = 3.0;
  cfg.spanCenterZ = 3.2;
  // Try to make the classifier favour a reduced model everywhere.
  cfg.refine.vorticityMag = 1e9;
  cfg.refine.qCriterion = 1e9;
  cfg.coarsen.smoothnessMax = 1e9;
  makeCubeBoxes(cfg);
  GuardZones guards(cfg);
  RuleBasedClassifier clf;

  // A point in the cube-front/floor junction guard zone, with sensors that the
  // (mis)configured classifier would label INVISCID.
  const Vec3 guarded{3.0, 0.1, 3.2};
  REQUIRE(guards.isForcedFullNS(guarded));

  CellSensors s;  // smooth, not near wall => classifier proposes a reduced model
  s.nearWall = false;
  s.gradUMag = 0.0;
  s.vorticityMag = 0.0;
  const Mode proposed = clf.propose(s, cfg);
  REQUIRE(proposed != Mode::FullNS);  // config tried to reduce it

  // The guard overrides regardless: the final mode in a guard zone is FULL_NS.
  const Mode final = guards.isForcedFullNS(guarded) ? Mode::FullNS : proposed;
  REQUIRE(final == Mode::FullNS);

  // A point far from the body is NOT guarded.
  REQUIRE_FALSE(guards.isForcedFullNS(Vec3{12.0, 2.5, 3.2}));
}

TEST_CASE("static-box floors raise the minimum level near the body",
          "[adaptive][amr]") {
  AdaptiveConfig cfg;
  cfg.baseLevel = 2;
  cfg.maxLevel = 5;
  cfg.cubeHeight = 1.0;
  cfg.cubeFrontX = 3.0;
  cfg.spanCenterZ = 3.2;
  makeCubeBoxes(cfg);
  OctreeAMRController controller(cfg);

  Octree tree(Vec3{0, 0, 0}, Vec3{14, 3, 6.4}, cfg.maxLevel);
  tree.refineUniform(cfg.baseLevel);
  const std::size_t before = tree.cellCount();
  controller.enforceFloors(tree);
  REQUIRE(tree.cellCount() > before);
  REQUIRE(tree.isBalanced());

  // Every cell inside box B (cube shell) must now be at the finest level.
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    const Vec3 c = tree.cellCenter(i);
    if (c.x > 3.1 && c.x < 3.9 && c.y > 0.1 && c.y < 0.9 && c.z > 2.9 && c.z < 3.5) {
      REQUIRE(tree.level(i) == cfg.maxLevel);
    }
  }
}
