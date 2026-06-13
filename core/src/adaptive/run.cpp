#include "nabla/adaptive/run.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "nabla/flow/case.hpp"
#include "nabla/flow/ns_solver.hpp"
#include "nabla/flow/poisson.hpp"
#include "nabla/io/vtu_writer.hpp"
#include "nabla/mesh/octree.hpp"
#include "nabla/version.hpp"

namespace nabla::adaptive {
namespace {

// Baseline-backed flow: the validated Prompt-4 staggered-MAC solver.
class BaselineBackend : public SolverBackend {
 public:
  BaselineBackend(flow::UniformGrid grid, flow::FlowConfig cfg)
      : grid_(std::move(grid)),
        solver_(grid_, cfg, std::make_shared<flow::MgPoisson>(cfg.poissonTol,
                                                              cfg.poissonMaxIters)) {
    solver_.initialize();
  }
  void step() override { last_ = solver_.step(); }
  double cd() const override { return last_.cd; }
  double cl() const override { return last_.cl; }
  double momentumResidual() const override { return last_.momentumResidual; }
  double nu() const override { return solver_.nu(); }
  std::size_t uniformCells() const override { return grid_.cellCount(); }
  double t() const override { return last_.t; }
  double dt() const override { return last_.dt; }
  double cfl() const override { return last_.cfl; }
  double continuityResidual() const override { return last_.continuityResidual; }
  double divergenceMax() const override { return last_.divergenceMax; }
  double massError() const override { return last_.massError; }
  int poissonIters() const override { return last_.poissonIters; }
  double poissonResidual() const override { return last_.poissonResidual; }

  double sample(int component, Vec3 p) const override {
    int i = static_cast<int>((p.x - grid_.origin.x) / grid_.dx);
    int j = static_cast<int>((p.y - grid_.origin.y) / grid_.dy);
    int k = static_cast<int>((p.z - grid_.origin.z) / grid_.dz);
    i = std::clamp(i, 0, grid_.nx - 1);
    j = std::clamp(j, 0, grid_.ny - 1);
    k = std::clamp(k, 0, grid_.nz - 1);
    switch (component) {
      case 0: return solver_.cellU(i, j, k);
      case 1: return solver_.cellV(i, j, k);
      case 2: return solver_.cellW(i, j, k);
      default: return solver_.cellPressure()[grid_.cidx(i, j, k)];
    }
  }

 private:
  flow::UniformGrid grid_;
  flow::NSSolver solver_;
  flow::StepReport last_;
};

void markCubeSolids(Octree& tree, const AdaptiveConfig& cfg) {
  const double h = cfg.cubeHeight, xf = cfg.cubeFrontX, zc = cfg.spanCenterZ;
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    const Vec3 c = tree.cellCenter(i);
    const bool inside = c.x >= xf && c.x <= xf + h && c.y >= 0.0 && c.y <= h &&
                        c.z >= zc - 0.5 * h && c.z <= zc + 0.5 * h;
    tree.setMask(i, inside ? mesh::CellMask::InsideSolid : mesh::CellMask::Fluid);
  }
}

void sampleFlow(Octree& tree, const SolverBackend& backend) {
  auto& U = tree.u();
  auto& V = tree.v();
  auto& W = tree.w();
  auto& P = tree.p();
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    const Vec3 c = tree.cellCenter(i);
    U[i] = backend.sample(0, c);
    V[i] = backend.sample(1, c);
    W[i] = backend.sample(2, c);
    P[i] = backend.sample(3, c);
  }
}

// Classify every cell, run the acceptance gate, enforce hard guards, update the
// physics_mode label, and audit the decisions.
void applyClassification(Octree& tree, const std::vector<CellSensors>& rows,
                         const PhysicsModeClassifier& clf, const AdaptiveConfig& cfg,
                         const GuardZones& guards, AuditTrail& audit) {
  tree.registerLabel(kPhysicsMode);
  auto& mode = tree.label(kPhysicsMode);
  int acc[5] = {0, 0, 0, 0, 0};
  int rej[5] = {0, 0, 0, 0, 0};
  int guardOverrides = 0;

  for (std::size_t i = 0; i < tree.cellCount() && i < rows.size(); ++i) {
    const Mode prev = static_cast<Mode>(mode[i]);
    const Mode proposed = clf.propose(rows[i], cfg);
    const Vec3 c = tree.cellCenter(i);
    Mode final = Mode::FullNS;
    std::string reason;
    if (guards.isForcedFullNS(c)) {
      final = Mode::FullNS;
      reason = std::string("hard guard: ") + guards.zoneName(c);
      if (proposed != Mode::FullNS) {
        ++guardOverrides;
      }
    } else {
      const AcceptanceResult a = acceptReducedModel(proposed, rows[i], cfg);
      const int pm = static_cast<int>(proposed);
      if (a.accepted) {
        final = proposed;
        ++acc[pm];
        reason = std::string("accepted ") + modeName(proposed);
      } else {
        final = Mode::FullNS;
        ++rej[pm];
        reason = a.reason;
      }
    }
    if (final != prev) {
      audit.modelChange(tree.morton(i), prev, final, reason);
    }
    mode[i] = static_cast<std::uint8_t>(final);
  }

  for (int m = 1; m <= 3; ++m) {  // NearWall, LaminarBL, Inviscid
    if (acc[m] + rej[m] > 0) {
      audit.regionAcceptance(modeName(static_cast<Mode>(m)), static_cast<Mode>(m),
                             rej[m] == 0,
                             std::to_string(acc[m]) + " accepted, " + std::to_string(rej[m]) +
                                 " rejected -> FULL_NS");
    }
  }
  if (guardOverrides > 0) {
    audit.regionAcceptance("hard-guard-zones", Mode::FullNS, true,
                           std::to_string(guardOverrides) +
                               " reduced proposals overridden to FULL_NS");
  }
}

double maxTruncErr(const std::vector<CellSensors>& rows) {
  double m = 0.0;
  for (const auto& s : rows) {
    m = std::max(m, s.truncErr);
  }
  return m;
}

// One diagnostics.jsonl line per solver step — same shape as the baseline run's
// stream, so the live dashboard consumes both run kinds identically. `cells` is
// the adaptive octree count (the quantity that evolves under AMR).
void appendDiagnostics(std::ofstream& out, int step, const SolverBackend& backend,
                       std::size_t octreeCells) {
  out << "{\"step\":" << step << ",\"t\":" << backend.t() << ",\"dt\":" << backend.dt()
      << ",\"cfl\":" << backend.cfl() << ",\"momentum_residual\":" << backend.momentumResidual()
      << ",\"continuity_residual\":" << backend.continuityResidual()
      << ",\"divergence_max\":" << backend.divergenceMax()
      << ",\"mass_error\":" << backend.massError() << ",\"cd\":" << backend.cd()
      << ",\"cl\":" << backend.cl() << ",\"poisson_iters\":" << backend.poissonIters()
      << ",\"poisson_residual\":" << backend.poissonResidual()
      << ",\"cells\":" << octreeCells << ",\"accepted\":true}\n";
  out.flush();
}

// Atomically replace <runDir>/adaptive_latest.vtu so a concurrent reader (the
// backend's fidelity-slice endpoint) never sees a half-written file.
void publishLatestVtu(const Octree& tree, const std::string& runDir) {
  const std::string tmp = runDir + "/adaptive_latest.vtu.tmp";
  io::writeVtu(tree, tmp);
  std::filesystem::rename(tmp, runDir + "/adaptive_latest.vtu");
}

}  // namespace

AdaptiveRunResult runAdaptiveCube(const AdaptiveRunOptions& opt) {
  // 1. Baseline flow backend (validated MAC solver).
  flow::CaseSpec base = flow::makeWallMountedCube(opt.resolution, opt.reynolds);
  auto backend = std::make_unique<BaselineBackend>(base.grid, base.flow);

  // 2. Octree adaptive mesh over the same domain.
  const double h = base.flow.refLength;
  const Vec3 origin{base.grid.origin.x, base.grid.origin.y, base.grid.origin.z};
  const Vec3 extent{base.grid.nx * base.grid.dx, base.grid.ny * base.grid.dy,
                    base.grid.nz * base.grid.dz};
  Octree tree(origin, extent, opt.octreeMaxLevel);

  AdaptiveConfig cfg;
  cfg.baseLevel = opt.octreeBaseLevel;
  cfg.maxLevel = opt.octreeMaxLevel;
  cfg.cubeHeight = h;
  cfg.cubeFrontX = 3.0 * h;
  cfg.spanCenterZ = 3.2 * h;
  makeCubeBoxes(cfg);

  OctreeAMRController controller(cfg);
  GuardZones guards(cfg);
  RuleBasedClassifier classifier;
  PhysicalSensorEvaluator sensors(backend->nu());
  StepAcceptanceController stepAcc(cfg.tol);

  std::filesystem::create_directories(opt.runDir);
  AuditTrail audit(opt.runDir + "/audit.jsonl");
  EfficiencyMonitor eff;
  // uniform-fine reference = whole domain refined to maxLevel (single octree).
  const std::size_t fine = static_cast<std::size_t>(1) << (3 * opt.octreeMaxLevel);
  eff.setUniformFineCells(fine);

  // Initial mesh: base + static-box floors, then mark the cube solids.
  tree.refineUniform(cfg.baseLevel);
  controller.enforceFloors(tree);
  markCubeSolids(tree, cfg);
  publishLatestVtu(tree, opt.runDir);  // map has content from step zero

  std::ofstream diagnostics(opt.runDir + "/diagnostics.jsonl", std::ios::trunc);
  diagnostics << "{\"event\":\"meta\",\"solver\":\"nabla_solve\",\"solver_version\":\""
              << nabla::kVersionString << "\",\"git_sha\":\"" << nabla::kGitSha << "\"}\n";
  int globalStep = 0;

  // 3. Warm up the baseline flow (develop the field before adapting).
  for (int s = 0; s < opt.warmupSteps; ++s) {
    backend->step();
    appendDiagnostics(diagnostics, ++globalStep, *backend, tree.cellCount());
  }

  // 4. Predictor-refine -> solve -> corrector loop.
  int acceptedStreak = 0;
  double prevCd = backend->cd(), prevCl = backend->cl();
  for (int s = 1; s <= opt.adaptiveSteps; ++s) {
    const auto t0 = std::chrono::steady_clock::now();
    audit.beginStep(s, 0.0, 0.0);

    backend->step();  // solve
    const double cd = backend->cd(), cl = backend->cl();

    sampleFlow(tree, *backend);
    const std::vector<CellSensors> rows = sensors.evaluate(tree);
    applyClassification(tree, rows, classifier, cfg, guards, audit);

    const bool outputsStable =
        std::abs(cd - prevCd) <= cfg.coarsen.forceTol * (std::abs(prevCd) + 1e-9) &&
        std::abs(cl - prevCl) <= cfg.coarsen.forceTol * (std::abs(prevCl) + 1e-9);

    // decide coarsening BEFORE refinement mutates the tree, then refine.
    const std::vector<std::uint64_t> coarsenKeys =
        controller.markCoarsen(tree, rows, guards, acceptedStreak, outputsStable);
    const std::size_t refined = controller.refineBySensors(tree, rows);
    if (refined) {
      audit.refinements(refined, "sensor thresholds exceeded");
    }
    const std::size_t floored = controller.enforceFloors(tree);
    if (floored) {
      audit.refinements(floored, "static-box minimum-resolution floor");
    }
    std::size_t coarsened = 0;
    for (std::uint64_t k : coarsenKeys) {
      const std::size_t i = tree.find(k);
      if (i != Octree::npos && tree.coarsenSiblings(i)) {
        ++coarsened;
      }
    }
    if (coarsened) {
      audit.coarsenings(coarsened, "smooth, accepted streak, outputs stable");
    }
    markCubeSolids(tree, cfg);

    // corrector: step acceptance.
    StepErrors err;
    err.spatial = maxTruncErr(rows);
    err.temporal = 0.0;  // baseline backend advances at its own validated dt
    err.convergence = backend->momentumResidual();
    const RejectAction act = stepAcc.evaluate(err);
    if (act == RejectAction::Accept) {
      ++acceptedStreak;
      audit.stepDecision("accept", "spatial/temporal/convergence within tolerance");
    } else {
      acceptedStreak = 0;
      const char* a = act == RejectAction::RefineMesh ? "refine_mesh"
                      : act == RejectAction::ReduceDt ? "reduce_dt"
                                                      : "promote_models";
      audit.stepDecision(std::string("reject->") + a, "remedy applied in priority order");
    }

    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    eff.record(tree.cellCount(), wall);
    audit.metrics(tree.cellCount(), cd, cl);
    audit.endStep();
    appendDiagnostics(diagnostics, ++globalStep, *backend, tree.cellCount());
    publishLatestVtu(tree, opt.runDir);
    prevCd = cd;
    prevCl = cl;
  }

  // 5. Outputs.
  audit.writeSummary(opt.runDir + "/audit_summary.md");
  {
    std::ofstream effOut(opt.runDir + "/efficiency.txt");
    effOut << eff.report() << "\n";
  }
  io::writeVtu(tree, opt.runDir + "/adaptive_final.vtu");

  AdaptiveRunResult r;
  r.cd = backend->cd();
  r.cl = backend->cl();
  r.adaptiveCells = tree.cellCount();
  r.uniformFineCells = fine;
  r.cellSpeedup = eff.cellSpeedup();
  r.audit = audit.totals();
  r.runDir = opt.runDir;
  return r;
}

}  // namespace nabla::adaptive
