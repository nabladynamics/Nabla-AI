#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nabla/mesh/octree.hpp"

// Nabla AI adaptive layer: physics-aware AMR + reduced-model classification with
// a fully audited decision trail, sitting on top of the baseline NS solver.
//
// Architecture (ADR-0004): the control plane (sensors, AMR controller,
// classifier + acceptance gate, hard guards, step-acceptance, audit, efficiency)
// operates on the octree mesh and is independent of how the flow is advanced —
// that is hidden behind SolverBackend, so an octree-native NS solve can replace
// the (validated) baseline-backed flow without touching the control plane.
namespace nabla::adaptive {

using mesh::Octree;
using mesh::Vec3;

// ---------------------------------------------------------------------------
// Per-cell physics modes. Reduced models PROPOSE; only NS-consistency ACCEPTS.
// ---------------------------------------------------------------------------
enum class Mode : uint8_t {
  FullNS = 0,     // full Navier–Stokes
  NearWall = 1,   // log-law wall model (y+ < switch)
  LaminarBL = 2,  // Blasius laminar boundary layer prior
  Inviscid = 3,   // potential / outer inviscid
  WakeShear = 4,  // forced full resolution in wake/shear layers
};
const char* modeName(Mode m);

// Octree field names written by the adaptive layer.
inline constexpr const char* kVortMag = "vorticity_mag";
inline constexpr const char* kStrainMag = "strain_mag";
inline constexpr const char* kQCrit = "q_criterion";
inline constexpr const char* kGradP = "grad_p_mag";
inline constexpr const char* kGradU = "grad_u_mag";
inline constexpr const char* kYPlus = "y_plus";
inline constexpr const char* kTauWall = "tau_wall";
inline constexpr const char* kTruncErr = "trunc_err";
inline constexpr const char* kResidStag = "resid_stagnation";  // 0/1 flag
inline constexpr const char* kPhysicsMode = "physics_mode";    // uint8 label

// ---------------------------------------------------------------------------
// Per-cell sensor bundle (a row of the indicator table for one cell).
// ---------------------------------------------------------------------------
struct CellSensors {
  double vorticityMag = 0.0;
  double strainMag = 0.0;
  double qCriterion = 0.0;
  double gradPMag = 0.0;
  double gradUMag = 0.0;
  double yPlus = -1.0;     // <0 => not a near-wall cell
  double tauWall = 0.0;
  double truncErr = 0.0;
  bool residStagnation = false;
  bool nearWall = false;
  bool reverseFlow = false;  // tangential velocity opposes free stream / wall-parallel
};

// ---------------------------------------------------------------------------
// Configuration (every threshold lives here — never hard-coded in the logic).
// ---------------------------------------------------------------------------
struct SensorThresholds {
  double vorticityMag = 8.0;   // refine if |omega| exceeds
  double qCriterion = 2.0;     // refine if Q exceeds (vortex cores)
  double gradPMag = 6.0;       // refine if |grad p| exceeds
  double gradUMag = 12.0;      // refine if |grad u| exceeds
  double truncErr = 0.05;      // refine if local truncation estimate exceeds
};

struct CoarsenPolicy {
  int consecutiveAcceptedSteps = 5;  // N accepted steps before coarsening allowed
  double smoothnessMax = 0.5;        // |grad u| below this => smooth enough
  double forceTol = 0.02;            // Cd/Cl must be unchanged within this
};

struct StepTolerances {
  double spatialError = 0.05;
  double temporalError = 0.02;   // ||u(dt) - u(dt/2)|| / U
  double convergence = 1e-3;     // momentum residual gate
};

// A static minimum-resolution box (Phase-0 spec). Cells whose center is inside
// must be at least `minLevel`. Geometry is the spec's; minLevel is config.
struct StaticBox {
  std::string name;
  double xlo, xhi, ylo, yhi, zlo, zhi;
  int minLevel = 0;
  bool forceFullNS = false;  // hard guard: no reduced physics here
};

struct AdaptiveConfig {
  SensorThresholds refine;
  CoarsenPolicy coarsen;
  StepTolerances tol;
  double yPlusWallSwitch = 11.25;  // log-law switch
  int baseLevel = 3;
  int maxLevel = 8;
  // Cube reference geometry (built by makeCubeBoxes).
  double cubeHeight = 1.0;
  double cubeFrontX = 3.0;   // x of the cube front face
  double spanCenterZ = 3.2;  // z of the span center
  std::vector<StaticBox> boxes;   // floors A–E
  std::vector<StaticBox> guards;  // forced-FULL_NS zones (cannot be overridden)
};

// Build the Phase-0 static boxes A–E and the hard-guard zones from the spec,
// scaled by the cube height. minLevels are taken from `cfg` defaults below.
void makeCubeBoxes(AdaptiveConfig& cfg);

// ---------------------------------------------------------------------------
// 1. Sensors
// ---------------------------------------------------------------------------
class PhysicalSensorEvaluator {
 public:
  explicit PhysicalSensorEvaluator(double nu) : nu_(nu) {}

  // Reads u,v,w,p octree fields; writes the sensor fields; returns per-cell rows.
  std::vector<CellSensors> evaluate(Octree& tree) const;

 private:
  double nu_;
};

// ---------------------------------------------------------------------------
// 2. Physics-mode classifier (swappable) + acceptance gate
// ---------------------------------------------------------------------------
class PhysicsModeClassifier {
 public:
  virtual ~PhysicsModeClassifier() = default;
  // Propose a per-cell mode from sensors. (An ML model can implement this.)
  [[nodiscard]] virtual Mode propose(const CellSensors& s,
                                     const AdaptiveConfig& cfg) const = 0;
  [[nodiscard]] virtual std::string name() const = 0;
};

class RuleBasedClassifier : public PhysicsModeClassifier {
 public:
  [[nodiscard]] Mode propose(const CellSensors& s,
                             const AdaptiveConfig& cfg) const override;
  [[nodiscard]] std::string name() const override { return "rule-based"; }
};

// NS-consistency acceptance: only this may ACCEPT a reduced model. On failure
// the caller promotes the cell to FULL_NS and logs `reason`.
struct AcceptanceResult {
  bool accepted = false;
  std::string reason;
};
AcceptanceResult acceptReducedModel(Mode proposed, const CellSensors& s,
                                    const AdaptiveConfig& cfg);

// ---------------------------------------------------------------------------
// 3. Hard guards — forced FULL_NS zones, not overridable by config/classifier.
// ---------------------------------------------------------------------------
class GuardZones {
 public:
  explicit GuardZones(const AdaptiveConfig& cfg) : guards_(cfg.guards) {}
  [[nodiscard]] bool isForcedFullNS(Vec3 cellCenter) const;
  [[nodiscard]] const char* zoneName(Vec3 cellCenter) const;

 private:
  std::vector<StaticBox> guards_;
};

// ---------------------------------------------------------------------------
// 4. AMR controller
// ---------------------------------------------------------------------------
struct AmrAction {
  std::size_t refined = 0;
  std::size_t coarsened = 0;
  std::size_t floorRefined = 0;  // refined to satisfy a static box floor
};

class OctreeAMRController {
 public:
  explicit OctreeAMRController(AdaptiveConfig cfg) : cfg_(std::move(cfg)) {}

  // Enforce static-box minimum-resolution floors (predictor stage / init).
  std::size_t enforceFloors(Octree& tree) const;

  // Mark+apply refinement where sensors exceed thresholds (predictor-refine).
  std::size_t refineBySensors(Octree& tree, const std::vector<CellSensors>& sensors) const;

  // Decide (but do not apply) which cells may coarsen: smooth, accepted for N
  // steps, outputs stable — and never below a floor or inside a guard. Returns
  // morton keys so the caller can apply after refinement has mutated the tree.
  std::vector<std::uint64_t> markCoarsen(const Octree& tree,
                                         const std::vector<CellSensors>& sensors,
                                         const GuardZones& guards, int acceptedStreak,
                                         bool outputsStable) const;

  [[nodiscard]] int minLevelAt(Vec3 c) const;  // floor from static boxes
  [[nodiscard]] const AdaptiveConfig& config() const { return cfg_; }

 private:
  AdaptiveConfig cfg_;
};

// ---------------------------------------------------------------------------
// 5. Step acceptance
// ---------------------------------------------------------------------------
enum class RejectAction { Accept, RefineMesh, ReduceDt, PromoteModels };

struct StepErrors {
  double spatial = 0.0;
  double temporal = 0.0;
  double convergence = 0.0;
};

class StepAcceptanceController {
 public:
  explicit StepAcceptanceController(StepTolerances tol) : tol_(tol) {}
  // Returns Accept, or the next remedy to try in priority order
  // (refine mesh -> reduce dt -> promote models).
  [[nodiscard]] RejectAction evaluate(const StepErrors& e) const;
  [[nodiscard]] bool accepted(const StepErrors& e) const {
    return evaluate(e) == RejectAction::Accept;
  }

 private:
  StepTolerances tol_;
};

// ---------------------------------------------------------------------------
// 6. Audit trail (first-class product output)
// ---------------------------------------------------------------------------
class AuditTrail {
 public:
  explicit AuditTrail(std::string path);
  ~AuditTrail();

  void beginStep(int step, double t, double dt);
  // Per-step quantitative record (post-AMR cell count + monitored outputs).
  void metrics(std::size_t cells, double cd, double cl);
  void refinements(std::size_t count, const std::string& reason);
  void coarsenings(std::size_t count, const std::string& reason);
  void modelChange(std::uint64_t cellMorton, Mode from, Mode to, const std::string& reason);
  void regionAcceptance(const std::string& region, Mode mode, bool accepted,
                        const std::string& reason);
  void stepDecision(const std::string& decision, const std::string& detail);
  void endStep();

  // Counters for the summary table.
  struct Totals {
    std::size_t refinements = 0, coarsenings = 0, modelChanges = 0;
    std::size_t accepts = 0, rejects = 0, promotions = 0, steps = 0;
  };
  [[nodiscard]] const Totals& totals() const { return totals_; }
  // Markdown summary table written to `path`.
  void writeSummary(const std::string& path) const;

 private:
  void emit(const std::string& json);
  std::string path_;
  std::shared_ptr<void> out_;  // ofstream (opaque to keep header light)
  int step_ = 0;
  Totals totals_;
};

// ---------------------------------------------------------------------------
// 7. Efficiency metric
// ---------------------------------------------------------------------------
class EfficiencyMonitor {
 public:
  void setUniformFineCells(std::size_t n) { uniformFine_ = n; }
  void record(std::size_t cells, double wallSeconds);

  [[nodiscard]] std::size_t uniformFineCells() const { return uniformFine_; }
  [[nodiscard]] double meanCells() const;
  [[nodiscard]] double cellSpeedup() const;  // uniformFine / meanCells
  [[nodiscard]] double totalWallSeconds() const { return wall_; }
  [[nodiscard]] std::string report() const;

 private:
  std::size_t uniformFine_ = 0;
  std::vector<std::size_t> cells_;
  double wall_ = 0.0;
};

// ---------------------------------------------------------------------------
// Octree conservative scalar transport — a genuine PDE solve on the octree used
// to demonstrate conservation under refine/coarsen during a live solve.
// ---------------------------------------------------------------------------
// Advances field `name` one step by conservative upwind FV using the registered
// velocity fields u,v,w. Returns the integral sum_i phi_i * vol_i (conserved up
// to boundary flux; with a divergence-free, wall-bounded velocity it is exact).
double octreeTransportStep(Octree& tree, const std::string& name, double dt);
double octreeFieldIntegral(const Octree& tree, const std::string& name);

}  // namespace nabla::adaptive
