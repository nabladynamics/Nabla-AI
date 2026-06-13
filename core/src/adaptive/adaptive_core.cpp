#include "nabla/adaptive/adaptive.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nabla::adaptive {

const char* modeName(Mode m) {
  switch (m) {
    case Mode::FullNS: return "FULL_NS";
    case Mode::NearWall: return "NEAR_WALL";
    case Mode::LaminarBL: return "LAMINAR_BL";
    case Mode::Inviscid: return "INVISCID";
    case Mode::WakeShear: return "WAKE_SHEAR";
  }
  return "FULL_NS";
}

// ---------------------------------------------------------------------------
// Static boxes A–E and hard-guard zones (Phase-0 spec, scaled by cube height).
// ---------------------------------------------------------------------------
void makeCubeBoxes(AdaptiveConfig& cfg) {
  const double h = cfg.cubeHeight;
  const double xf = cfg.cubeFrontX;     // cube front face
  const double zc = cfg.spanCenterZ;    // span center
  const int Lmax = cfg.maxLevel;
  cfg.boxes.clear();
  cfg.guards.clear();

  auto box = [&](const char* nm, double x0, double x1, double y0, double y1, double z0,
                 double z1, int lvl, bool guard) {
    StaticBox b;
    b.name = nm;
    b.xlo = x0; b.xhi = x1; b.ylo = y0; b.yhi = y1; b.zlo = z0; b.zhi = z1;
    b.minLevel = lvl;
    b.forceFullNS = guard;
    return b;
  };

  // Minimum-resolution floors (x ranges relative to the cube front face).
  cfg.boxes.push_back(box("A_horseshoe", xf - 2.5 * h, xf, 0.0, 1.2 * h, zc - 1.8 * h,
                          zc + 1.8 * h, Lmax - 1, false));
  cfg.boxes.push_back(box("B_cube_shell", xf - 0.3 * h, xf + 1.3 * h, 0.0, 1.3 * h,
                          zc - 0.8 * h, zc + 0.8 * h, Lmax, false));
  cfg.boxes.push_back(box("C_top_side_shear", xf, xf + 2.5 * h, 0.0, 1.8 * h, zc - 1.8 * h,
                          zc + 1.8 * h, Lmax - 1, false));
  cfg.boxes.push_back(box("D_near_wake", xf + 1.0 * h, xf + 6.0 * h, 0.0, 2.0 * h,
                          zc - 2.5 * h, zc + 2.5 * h, Lmax - 1, false));
  cfg.boxes.push_back(box("E_far_wake_monitor", xf + 6.0 * h, xf + 10.0 * h, 0.0, 2.5 * h,
                          zc - 2.5 * h, zc + 2.5 * h, cfg.baseLevel + 1, false));

  // Hard guards — forced FULL_NS, finest resolution, not overridable.
  cfg.guards.push_back(box("guard_horseshoe_birth", xf - 0.6 * h, xf, 0.0, 0.35 * h,
                           zc - 0.7 * h, zc + 0.7 * h, Lmax, true));
  cfg.guards.push_back(box("guard_front_floor_junction", xf - 0.15 * h, xf + 0.15 * h, 0.0,
                           0.3 * h, zc - 0.6 * h, zc + 0.6 * h, Lmax, true));
  cfg.guards.push_back(box("guard_leading_edges", xf - 0.1 * h, xf + 0.2 * h, 0.0, 1.05 * h,
                           zc - 0.6 * h, zc + 0.6 * h, Lmax, true));
  cfg.guards.push_back(box("guard_near_wake_start", xf + 1.0 * h, xf + 2.0 * h, 0.0, 1.3 * h,
                           zc - 0.8 * h, zc + 0.8 * h, Lmax, true));
}

namespace {
bool inBox(const StaticBox& b, Vec3 c) {
  return c.x >= b.xlo && c.x <= b.xhi && c.y >= b.ylo && c.y <= b.yhi && c.z >= b.zlo &&
         c.z <= b.zhi;
}
}  // namespace

// ---------------------------------------------------------------------------
// 1. Sensors
// ---------------------------------------------------------------------------
std::vector<CellSensors> PhysicalSensorEvaluator::evaluate(Octree& tree) const {
  // Register all output fields FIRST — registerScalar can reallocate the field
  // storage, which would dangle any references captured beforehand.
  tree.registerScalar(kVortMag);
  tree.registerScalar(kStrainMag);
  tree.registerScalar(kQCrit);
  tree.registerScalar(kGradP);
  tree.registerScalar(kGradU);
  tree.registerScalar(kYPlus);
  tree.registerScalar(kTauWall);
  tree.registerScalar(kTruncErr);
  tree.registerScalar(kResidStag);
  tree.registerScalar("speed_prev");
  tree.registerScalar("resid_prev");

  const auto& U = tree.u();
  const auto& V = tree.v();
  const auto& W = tree.w();
  const auto& P = tree.p();

  const std::size_t n = tree.cellCount();
  std::vector<CellSensors> rows(n);

  for (std::size_t i = 0; i < n; ++i) {
    const Vec3 du = tree.gradient(U, i);
    const Vec3 dv = tree.gradient(V, i);
    const Vec3 dw = tree.gradient(W, i);
    const Vec3 dp = tree.gradient(P, i);

    // velocity gradient tensor -> vorticity, strain, Q
    const Vec3 omega{dw.y - dv.z, du.z - dw.x, dv.x - du.y};
    const double vortMag = std::sqrt(omega.x * omega.x + omega.y * omega.y + omega.z * omega.z);
    const double Sxx = du.x, Syy = dv.y, Szz = dw.z;
    const double Sxy = 0.5 * (du.y + dv.x), Sxz = 0.5 * (du.z + dw.x), Syz = 0.5 * (dv.z + dw.y);
    const double SS = Sxx * Sxx + Syy * Syy + Szz * Szz + 2 * (Sxy * Sxy + Sxz * Sxz + Syz * Syz);
    const double Oxy = 0.5 * (du.y - dv.x), Oxz = 0.5 * (du.z - dw.x), Oyz = 0.5 * (dv.z - dw.y);
    const double OO = 2 * (Oxy * Oxy + Oxz * Oxz + Oyz * Oyz);
    const double strainMag = std::sqrt(2.0 * SS);
    const double q = 0.5 * (OO - SS);
    const double gradU = std::sqrt(du.x * du.x + du.y * du.y + du.z * du.z + dv.x * dv.x +
                                   dv.y * dv.y + dv.z * dv.z + dw.x * dw.x + dw.y * dw.y +
                                   dw.z * dw.z);
    const double gradP = std::sqrt(dp.x * dp.x + dp.y * dp.y + dp.z * dp.z);

    // near-wall: domain y-boundary (floor/ceiling) or a solid neighbor.
    bool nearWall = tree.faceNeighbors(i, 2).empty() || tree.faceNeighbors(i, 3).empty();
    for (int d = 0; d < 6 && !nearWall; ++d) {
      for (std::size_t nb : tree.faceNeighbors(i, d)) {
        if (tree.mask(nb) == mesh::CellMask::InsideSolid) {
          nearWall = true;
          break;
        }
      }
    }

    const Vec3 sz = tree.cellSize(i);
    const double speed = std::sqrt(U[i] * U[i] + V[i] * V[i] + W[i] * W[i]);
    double yPlus = -1.0, tauWall = 0.0;
    if (nearWall) {
      const double yw = 0.5 * sz.y;
      const double ut = std::sqrt(U[i] * U[i] + W[i] * W[i]);  // wall-parallel (floor)
      tauWall = nu_ * ut / (yw + 1e-300);
      const double uTau = std::sqrt(tauWall);
      yPlus = uTau * yw / (nu_ + 1e-300);
    }

    // Richardson-style truncation/smoothness estimate: deviation of the cell
    // value from the mean of its face neighbors (a coarse representation).
    double meanNb = 0.0;
    int cnt = 0;
    for (int d = 0; d < 6; ++d) {
      for (std::size_t nb : tree.faceNeighbors(i, d)) {
        meanNb += std::sqrt(U[nb] * U[nb] + V[nb] * V[nb] + W[nb] * W[nb]);
        ++cnt;
      }
    }
    const double truncErr = cnt ? std::abs(speed - meanNb / cnt) : 0.0;

    CellSensors& s = rows[i];
    s.vorticityMag = vortMag;
    s.strainMag = strainMag;
    s.qCriterion = q;
    s.gradPMag = gradP;
    s.gradUMag = gradU;
    s.yPlus = yPlus;
    s.tauWall = tauWall;
    s.truncErr = truncErr;
    s.nearWall = nearWall;
    s.reverseFlow = nearWall && (U[i] < 0.0);  // streamwise reversal => separation

    tree.scalar(kVortMag)[i] = vortMag;
    tree.scalar(kStrainMag)[i] = strainMag;
    tree.scalar(kQCrit)[i] = q;
    tree.scalar(kGradP)[i] = gradP;
    tree.scalar(kGradU)[i] = gradU;
    tree.scalar(kYPlus)[i] = yPlus;
    tree.scalar(kTauWall)[i] = tauWall;
    tree.scalar(kTruncErr)[i] = truncErr;
  }

  // residual-stagnation flag: per-cell unsteady residual that stops decreasing.
  auto& speedPrev = tree.scalar("speed_prev");
  auto& residPrev = tree.scalar("resid_prev");
  auto& residStag = tree.scalar(kResidStag);
  for (std::size_t i = 0; i < n; ++i) {
    const double speed = std::sqrt(U[i] * U[i] + V[i] * V[i] + W[i] * W[i]);
    const double resid = std::abs(speed - speedPrev[i]);
    const bool stag = residPrev[i] > 1e-9 && resid >= 0.95 * residPrev[i];
    rows[i].residStagnation = stag;
    residStag[i] = stag ? 1.0 : 0.0;
    residPrev[i] = resid;
    speedPrev[i] = speed;
  }
  return rows;
}

// ---------------------------------------------------------------------------
// 2. Classifier + acceptance gate
// ---------------------------------------------------------------------------
Mode RuleBasedClassifier::propose(const CellSensors& s, const AdaptiveConfig& cfg) const {
  // Vortical / shear-layer cells: forced full resolution.
  if (s.qCriterion > cfg.refine.qCriterion || s.vorticityMag > cfg.refine.vorticityMag) {
    return Mode::WakeShear;
  }
  // Near-wall: log-law region by y+.
  if (s.nearWall && s.yPlus >= 0.0 && s.yPlus < cfg.yPlusWallSwitch) {
    return Mode::NearWall;
  }
  if (s.nearWall) {
    return Mode::LaminarBL;  // attached BL beyond the log-law sublayer
  }
  // Smooth outer flow: candidate inviscid.
  if (s.gradUMag < cfg.coarsen.smoothnessMax && s.vorticityMag < 1.0) {
    return Mode::Inviscid;
  }
  return Mode::FullNS;
}

AcceptanceResult acceptReducedModel(Mode proposed, const CellSensors& s,
                                    const AdaptiveConfig& cfg) {
  switch (proposed) {
    case Mode::FullNS:
      return {true, "full NS (no reduction)"};
    case Mode::WakeShear:
      return {true, "wake/shear forced full resolution"};
    case Mode::NearWall:
      if (s.reverseFlow) {
        return {false, "reverse flow at wall: log-law assumption violated"};
      }
      if (s.yPlus < 0.0 || s.yPlus >= cfg.yPlusWallSwitch) {
        return {false, "y+ outside log-law validity"};
      }
      return {true, "attached log-law (y+ < switch, no reversal)"};
    case Mode::LaminarBL:
      if (s.reverseFlow) {
        return {false, "reverse flow: laminar BL prior invalid"};
      }
      if (s.vorticityMag > cfg.refine.vorticityMag) {
        return {false, "vorticity too high for laminar BL prior"};
      }
      return {true, "attached laminar boundary layer"};
    case Mode::Inviscid:
      if (s.nearWall) {
        return {false, "near-wall cell cannot be inviscid"};
      }
      if (s.vorticityMag > 1.0 || s.strainMag > cfg.refine.gradUMag) {
        return {false, "rotational/strained flow: inviscid invalid"};
      }
      return {true, "irrotational outer flow"};
  }
  return {false, "unknown mode"};
}

// ---------------------------------------------------------------------------
// 3. Hard guards
// ---------------------------------------------------------------------------
bool GuardZones::isForcedFullNS(Vec3 c) const {
  for (const StaticBox& g : guards_) {
    if (inBox(g, c)) {
      return true;
    }
  }
  return false;
}
const char* GuardZones::zoneName(Vec3 c) const {
  for (const StaticBox& g : guards_) {
    if (inBox(g, c)) {
      return g.name.c_str();
    }
  }
  return "";
}

// ---------------------------------------------------------------------------
// 4. AMR controller
// ---------------------------------------------------------------------------
int OctreeAMRController::minLevelAt(Vec3 c) const {
  int lvl = 0;
  for (const StaticBox& b : cfg_.boxes) {
    if (inBox(b, c)) {
      lvl = std::max(lvl, b.minLevel);
    }
  }
  for (const StaticBox& g : cfg_.guards) {
    if (inBox(g, c)) {
      lvl = std::max(lvl, g.minLevel);
    }
  }
  return lvl;
}

std::size_t OctreeAMRController::enforceFloors(Octree& tree) const {
  std::size_t total = 0;
  for (int pass = 0; pass < cfg_.maxLevel + 2; ++pass) {
    std::vector<uint64_t> toRefine;
    for (std::size_t i = 0; i < tree.cellCount(); ++i) {
      const int need = minLevelAt(tree.cellCenter(i));
      if (tree.level(i) < std::min(need, cfg_.maxLevel)) {
        toRefine.push_back(tree.morton(i));
      }
    }
    if (toRefine.empty()) {
      break;
    }
    for (uint64_t k : toRefine) {
      if (tree.find(k) != Octree::npos) {
        tree.refineByMorton(k);
        ++total;
      }
    }
  }
  return total;
}

std::size_t OctreeAMRController::refineBySensors(Octree& tree,
                                                const std::vector<CellSensors>& sensors) const {
  std::vector<uint64_t> toRefine;
  for (std::size_t i = 0; i < tree.cellCount() && i < sensors.size(); ++i) {
    if (tree.level(i) >= cfg_.maxLevel) {
      continue;
    }
    const CellSensors& s = sensors[i];
    const bool exceed = s.vorticityMag > cfg_.refine.vorticityMag ||
                        s.qCriterion > cfg_.refine.qCriterion ||
                        s.gradPMag > cfg_.refine.gradPMag ||
                        s.gradUMag > cfg_.refine.gradUMag ||
                        s.truncErr > cfg_.refine.truncErr;
    if (exceed) {
      toRefine.push_back(tree.morton(i));
    }
  }
  std::size_t count = 0;
  for (uint64_t k : toRefine) {
    if (tree.find(k) != Octree::npos) {
      tree.refineByMorton(k);
      ++count;
    }
  }
  return count;
}

std::vector<std::uint64_t> OctreeAMRController::markCoarsen(
    const Octree& tree, const std::vector<CellSensors>& sensors, const GuardZones& guards,
    int acceptedStreak, bool outputsStable) const {
  std::vector<std::uint64_t> keys;
  if (acceptedStreak < cfg_.coarsen.consecutiveAcceptedSteps || !outputsStable) {
    return keys;  // coarsening only after N accepted, output-stable steps
  }
  for (std::size_t i = 0; i < tree.cellCount() && i < sensors.size(); ++i) {
    if (tree.level(i) <= cfg_.baseLevel) {
      continue;
    }
    const Vec3 c = tree.cellCenter(i);
    if (guards.isForcedFullNS(c)) {
      continue;  // never coarsen a guard zone
    }
    // The parent's level must still satisfy the static floor.
    if (tree.level(i) - 1 < std::min(minLevelAt(c), cfg_.maxLevel)) {
      continue;
    }
    if (sensors[i].gradUMag >= cfg_.coarsen.smoothnessMax || sensors[i].vorticityMag >= 1.0) {
      continue;  // not smooth enough
    }
    keys.push_back(tree.morton(i));
  }
  return keys;
}

// ---------------------------------------------------------------------------
// 5. Step acceptance (remedy priority: refine mesh -> reduce dt -> promote)
// ---------------------------------------------------------------------------
RejectAction StepAcceptanceController::evaluate(const StepErrors& e) const {
  if (e.spatial > tol_.spatialError) {
    return RejectAction::RefineMesh;
  }
  if (e.temporal > tol_.temporalError) {
    return RejectAction::ReduceDt;
  }
  if (e.convergence > tol_.convergence) {
    return RejectAction::PromoteModels;
  }
  return RejectAction::Accept;
}

}  // namespace nabla::adaptive
