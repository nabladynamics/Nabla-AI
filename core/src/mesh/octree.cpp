#include "nabla/mesh/octree.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace nabla::mesh {
namespace {
constexpr std::array<const char*, 4> kCoreScalars = {"u", "v", "w", "p"};
}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
Octree::Octree(Vec3 origin, Vec3 extent, int maxLevel)
    : origin_(origin), extent_(extent), maxLevel_(maxLevel) {
  if (maxLevel < 1 || maxLevel > kMaxSupportedLevel) {
    throw std::invalid_argument("Octree: maxLevel must be in [1, 21]");
  }
  if (extent.x <= 0.0 || extent.y <= 0.0 || extent.z <= 0.0) {
    throw std::invalid_argument("Octree: extent must be positive");
  }
  const double inv = 1.0 / static_cast<double>(1u << maxLevel_);
  dxf_[0] = extent_.x * inv;
  dxf_[1] = extent_.y * inv;
  dxf_[2] = extent_.z * inv;

  for (const char* name : kCoreScalars) {
    registerScalar(name);
  }
  // Single root leaf covering the whole domain.
  appendCell(/*m=*/0, /*lvl=*/0, static_cast<uint8_t>(CellMask::Fluid));
}

Octree::Octree(const OctreeState& s)
    : origin_(s.origin), extent_(s.extent), maxLevel_(s.maxLevel) {
  if (maxLevel_ < 1 || maxLevel_ > kMaxSupportedLevel) {
    throw std::invalid_argument("Octree(state): invalid maxLevel");
  }
  const std::size_t n = s.morton.size();
  if (s.level.size() != n || s.mask.size() != n) {
    throw std::invalid_argument("Octree(state): inconsistent core array sizes");
  }
  const double inv = 1.0 / static_cast<double>(1u << maxLevel_);
  dxf_[0] = extent_.x * inv;
  dxf_[1] = extent_.y * inv;
  dxf_[2] = extent_.z * inv;

  morton_ = s.morton;
  level_ = s.level;
  mask_ = s.mask;

  dnames_ = s.scalarNames;
  dfields_ = s.scalars;
  for (std::size_t f = 0; f < dnames_.size(); ++f) {
    if (dfields_[f].size() != n) {
      throw std::invalid_argument("Octree(state): scalar field size mismatch");
    }
    dindex_[dnames_[f]] = static_cast<int>(f);
  }
  u8names_ = s.labelNames;
  u8fields_ = s.labels;
  for (std::size_t f = 0; f < u8names_.size(); ++f) {
    if (u8fields_[f].size() != n) {
      throw std::invalid_argument("Octree(state): label field size mismatch");
    }
    u8index_[u8names_[f]] = static_cast<int>(f);
  }
  rebuildIndex();
}

void Octree::rebuildIndex() {
  map_.clear();
  map_.reserve(morton_.size() * 2);
  for (std::size_t i = 0; i < morton_.size(); ++i) {
    map_[morton_[i]] = static_cast<uint32_t>(i);
  }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
double Octree::centerAxis(std::size_t i, int axis) const {
  uint32_t a[3];
  mortonDecode(morton_[i], a[0], a[1], a[2]);
  const uint32_t sz = cellSizeInt(level_[i]);
  const double anchorUnits = static_cast<double>(a[axis]) + 0.5 * static_cast<double>(sz);
  const double base = (axis == 0) ? origin_.x : (axis == 1 ? origin_.y : origin_.z);
  return base + anchorUnits * dxf_[axis];
}

Vec3 Octree::cellCenter(std::size_t i) const {
  return {centerAxis(i, 0), centerAxis(i, 1), centerAxis(i, 2)};
}

Vec3 Octree::cellSize(std::size_t i) const {
  const double s = static_cast<double>(cellSizeInt(level_[i]));
  return {s * dxf_[0], s * dxf_[1], s * dxf_[2]};
}

double Octree::cellVolume(std::size_t i) const {
  const Vec3 s = cellSize(i);
  return s.x * s.y * s.z;
}

std::size_t Octree::find(uint64_t mortonKey) const {
  const auto it = map_.find(mortonKey);
  return it == map_.end() ? npos : static_cast<std::size_t>(it->second);
}

// ---------------------------------------------------------------------------
// Field registry
// ---------------------------------------------------------------------------
int Octree::registerScalar(const std::string& name) {
  const auto it = dindex_.find(name);
  if (it != dindex_.end()) {
    return it->second;
  }
  const int idx = static_cast<int>(dfields_.size());
  dnames_.push_back(name);
  dindex_[name] = idx;
  dfields_.emplace_back(cellCount(), 0.0);
  return idx;
}

std::vector<double>& Octree::scalar(const std::string& name) {
  return dfields_[static_cast<std::size_t>(dindex_.at(name))];
}
const std::vector<double>& Octree::scalar(const std::string& name) const {
  return dfields_[static_cast<std::size_t>(dindex_.at(name))];
}

int Octree::registerLabel(const std::string& name) {
  const auto it = u8index_.find(name);
  if (it != u8index_.end()) {
    return it->second;
  }
  const int idx = static_cast<int>(u8fields_.size());
  u8names_.push_back(name);
  u8index_[name] = idx;
  u8fields_.emplace_back(cellCount(), uint8_t{0});
  return idx;
}

std::vector<uint8_t>& Octree::label(const std::string& name) {
  return u8fields_[static_cast<std::size_t>(u8index_.at(name))];
}
const std::vector<uint8_t>& Octree::label(const std::string& name) const {
  return u8fields_[static_cast<std::size_t>(u8index_.at(name))];
}

// ---------------------------------------------------------------------------
// Low-level SoA mutation
// ---------------------------------------------------------------------------
std::size_t Octree::appendCell(uint64_t m, int lvl, uint8_t mk) {
  const std::size_t i = morton_.size();
  morton_.push_back(m);
  level_.push_back(static_cast<uint8_t>(lvl));
  mask_.push_back(mk);
  for (auto& f : dfields_) {
    f.push_back(0.0);
  }
  for (auto& f : u8fields_) {
    f.push_back(0);
  }
  map_[m] = static_cast<uint32_t>(i);
  return i;
}

void Octree::swapRemove(std::size_t i) {
  const uint64_t removedKey = morton_[i];
  const std::size_t last = morton_.size() - 1;
  if (i != last) {
    morton_[i] = morton_[last];
    level_[i] = level_[last];
    mask_[i] = mask_[last];
    for (auto& f : dfields_) {
      f[i] = f[last];
    }
    for (auto& f : u8fields_) {
      f[i] = f[last];
    }
    map_[morton_[i]] = static_cast<uint32_t>(i);
  }
  morton_.pop_back();
  level_.pop_back();
  mask_.pop_back();
  for (auto& f : dfields_) {
    f.pop_back();
  }
  for (auto& f : u8fields_) {
    f.pop_back();
  }
  map_.erase(removedKey);
}

// ---------------------------------------------------------------------------
// Neighbor queries
// ---------------------------------------------------------------------------
std::size_t Octree::coarserOrEqualFaceNeighbor(uint32_t ax, uint32_t ay, uint32_t az,
                                               int lvl, int dir, int& outLevel) const {
  const int axis = dir / 2;
  const int sign = (dir & 1) ? +1 : -1;
  const uint32_t size = cellSizeInt(lvl);
  int64_t c[3] = {ax, ay, az};
  c[axis] += static_cast<int64_t>(sign) * static_cast<int64_t>(size);
  const int64_t domain = static_cast<int64_t>(1) << maxLevel_;
  if (c[axis] < 0 || c[axis] >= domain) {
    return npos;  // domain boundary
  }
  // Walk from this level up to the root; the first existing leaf at the masked
  // anchor is the unique coarser-or-equal cell containing that point.
  for (int lev = lvl; lev >= 0; --lev) {
    const uint32_t sz = cellSizeInt(lev);
    const uint32_t mask = ~(sz - 1u);
    const uint64_t key = mortonEncode(static_cast<uint32_t>(c[0]) & mask,
                                      static_cast<uint32_t>(c[1]) & mask,
                                      static_cast<uint32_t>(c[2]) & mask);
    const auto it = map_.find(key);
    if (it != map_.end() && level_[it->second] == lev) {
      outLevel = lev;
      return static_cast<std::size_t>(it->second);
    }
  }
  return npos;  // neighbor is finer than `lvl`
}

std::vector<std::size_t> Octree::faceNeighborsOf(uint32_t ax, uint32_t ay, uint32_t az,
                                                 int lvl, int dir) const {
  const int axis = dir / 2;
  const int sign = (dir & 1) ? +1 : -1;
  const uint32_t size = cellSizeInt(lvl);
  int64_t c[3] = {ax, ay, az};
  c[axis] += static_cast<int64_t>(sign) * static_cast<int64_t>(size);
  const int64_t domain = static_cast<int64_t>(1) << maxLevel_;
  if (c[axis] < 0 || c[axis] >= domain) {
    return {};
  }
  uint32_t n[3] = {static_cast<uint32_t>(c[0]), static_cast<uint32_t>(c[1]),
                   static_cast<uint32_t>(c[2])};

  // Coarser-or-equal: a single neighbor.
  for (int lev = lvl; lev >= 0; --lev) {
    const uint32_t sz = cellSizeInt(lev);
    const uint32_t mask = ~(sz - 1u);
    const uint64_t key = mortonEncode(n[0] & mask, n[1] & mask, n[2] & mask);
    const auto it = map_.find(key);
    if (it != map_.end() && level_[it->second] == lev) {
      return {static_cast<std::size_t>(it->second)};
    }
  }

  // Finer: the 2x2 children (level lvl+1) on the touching side of the face.
  std::vector<std::size_t> out;
  if (lvl + 1 > maxLevel_) {
    return out;
  }
  const uint32_t csz = cellSizeInt(lvl + 1);
  const int t1 = (axis + 1) % 3;
  const int t2 = (axis + 2) % 3;
  const uint32_t axisAnchor = (sign > 0) ? n[axis] : (n[axis] + size - csz);
  for (int k1 = 0; k1 < 2; ++k1) {
    for (int k2 = 0; k2 < 2; ++k2) {
      uint32_t a[3];
      a[axis] = axisAnchor;
      a[t1] = n[t1] + static_cast<uint32_t>(k1) * csz;
      a[t2] = n[t2] + static_cast<uint32_t>(k2) * csz;
      const uint64_t key = mortonEncode(a[0], a[1], a[2]);
      const auto it = map_.find(key);
      if (it != map_.end() && level_[it->second] == lvl + 1) {
        out.push_back(static_cast<std::size_t>(it->second));
      }
    }
  }
  return out;
}

std::vector<std::size_t> Octree::faceNeighbors(std::size_t i, int dir) const {
  uint32_t ax, ay, az;
  anchorOf(i, ax, ay, az);
  return faceNeighborsOf(ax, ay, az, level_[i], dir);
}

// ---------------------------------------------------------------------------
// Refinement (with automatic 2:1 face balance)
// ---------------------------------------------------------------------------
void Octree::splitLeaf(std::size_t i) {
  const uint64_t pm = morton_[i];
  const int L = level_[i];
  const uint8_t mk = mask_[i];
  uint32_t ax, ay, az;
  mortonDecode(pm, ax, ay, az);
  const uint32_t csz = cellSizeInt(L + 1);

  // Capture parent field values before the SoA is mutated.
  std::vector<double> pd(dfields_.size());
  for (std::size_t f = 0; f < dfields_.size(); ++f) {
    pd[f] = dfields_[f][i];
  }
  std::vector<uint8_t> pu(u8fields_.size());
  for (std::size_t f = 0; f < u8fields_.size(); ++f) {
    pu[f] = u8fields_[f][i];
  }

  swapRemove(i);
  for (int c = 0; c < 8; ++c) {
    const uint32_t cx = static_cast<uint32_t>(c & 1);
    const uint32_t cy = static_cast<uint32_t>((c >> 1) & 1);
    const uint32_t cz = static_cast<uint32_t>((c >> 2) & 1);
    const uint64_t cm = mortonEncode(ax + cx * csz, ay + cy * csz, az + cz * csz);
    const std::size_t ni = appendCell(cm, L + 1, mk);
    // Conservative interpolation: piecewise-constant injection. The integral
    // over the parent (value * volume) is preserved exactly because the 8
    // children partition the parent volume.
    for (std::size_t f = 0; f < dfields_.size(); ++f) {
      dfields_[f][ni] = pd[f];
    }
    for (std::size_t f = 0; f < u8fields_.size(); ++f) {
      u8fields_[f][ni] = pu[f];
    }
  }
}

void Octree::refineByMorton(uint64_t mortonKey) {
  if (map_.find(mortonKey) == map_.end()) {
    throw std::runtime_error("refine: cell not found");
  }
  // Enforce 2:1 balance: repeatedly refine any face neighbor that is coarser
  // than this cell, until none remain, then split.
  for (;;) {
    const auto it = map_.find(mortonKey);
    const std::size_t i = it->second;
    const int L = level_[i];
    if (L >= maxLevel_) {
      throw std::runtime_error("refine: cell already at maxLevel");
    }
    uint32_t ax, ay, az;
    mortonDecode(mortonKey, ax, ay, az);
    bool refinedNeighbor = false;
    for (int dir = 0; dir < 6; ++dir) {
      int nlvl = 0;
      const std::size_t nb = coarserOrEqualFaceNeighbor(ax, ay, az, L, dir, nlvl);
      if (nb != npos && nlvl < L) {
        const uint64_t nkey = morton_[nb];
        refineByMorton(nkey);
        refinedNeighbor = true;
        break;
      }
    }
    if (!refinedNeighbor) {
      break;
    }
  }
  splitLeaf(map_.find(mortonKey)->second);
}

void Octree::refine(std::size_t cellIndex) { refineByMorton(morton_[cellIndex]); }

void Octree::refineUniform(int times) {
  for (int t = 0; t < times; ++t) {
    const std::vector<uint64_t> keys(morton_.begin(), morton_.end());
    for (const uint64_t k : keys) {
      const auto it = map_.find(k);
      if (it != map_.end()) {
        splitLeaf(it->second);  // uniform refinement preserves balance
      }
    }
  }
}

void Octree::refineBox(Vec3 lo, Vec3 hi, int times) {
  for (int t = 0; t < times; ++t) {
    std::vector<uint64_t> keys;
    for (std::size_t i = 0; i < cellCount(); ++i) {
      const Vec3 c = cellCenter(i);
      if (c.x >= lo.x && c.x <= hi.x && c.y >= lo.y && c.y <= hi.y && c.z >= lo.z &&
          c.z <= hi.z) {
        keys.push_back(morton_[i]);
      }
    }
    for (const uint64_t k : keys) {
      if (map_.find(k) != map_.end()) {
        refineByMorton(k);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Coarsening (volume-weighted averaging, balance-preserving)
// ---------------------------------------------------------------------------
bool Octree::coarsen(uint64_t parentMorton, int parentLevel) {
  if (parentLevel < 0 || parentLevel >= maxLevel_) {
    return false;
  }
  uint32_t ax, ay, az;
  mortonDecode(parentMorton, ax, ay, az);
  const uint32_t psz = cellSizeInt(parentLevel);
  if ((ax & (psz - 1u)) || (ay & (psz - 1u)) || (az & (psz - 1u))) {
    return false;  // not a valid parent anchor for this level
  }
  const uint32_t csz = cellSizeInt(parentLevel + 1);

  // All 8 children must exist as leaves at parentLevel+1.
  std::array<std::size_t, 8> child{};
  for (int c = 0; c < 8; ++c) {
    const uint32_t cx = static_cast<uint32_t>(c & 1);
    const uint32_t cy = static_cast<uint32_t>((c >> 1) & 1);
    const uint32_t cz = static_cast<uint32_t>((c >> 2) & 1);
    const uint64_t cm = mortonEncode(ax + cx * csz, ay + cy * csz, az + cz * csz);
    const auto it = map_.find(cm);
    if (it == map_.end() || level_[it->second] != parentLevel + 1) {
      return false;
    }
    child[static_cast<std::size_t>(c)] = it->second;
  }

  // Balance safety: refuse if any child has a face neighbor finer than itself
  // (level > parentLevel+1). Merging the family to parentLevel would otherwise
  // leave that finer cell two levels away. We must query from the children's
  // faces — a parent-level query can only see down to parentLevel+1 and would
  // miss the offending parentLevel+2 neighbors.
  for (int c = 0; c < 8; ++c) {
    const std::size_t ci = child[static_cast<std::size_t>(c)];
    for (int dir = 0; dir < 6; ++dir) {
      for (const std::size_t nb : faceNeighbors(ci, dir)) {
        if (level_[nb] > parentLevel + 1) {
          return false;
        }
      }
    }
  }

  // Volume-weighted average (children have equal volume => arithmetic mean).
  std::vector<double> avg(dfields_.size(), 0.0);
  for (std::size_t f = 0; f < dfields_.size(); ++f) {
    double s = 0.0;
    for (int c = 0; c < 8; ++c) {
      s += dfields_[f][child[static_cast<std::size_t>(c)]];
    }
    avg[f] = s / 8.0;
  }
  // Labels: inherit child 0; mask: uniform if all children agree, else Cut.
  std::vector<uint8_t> lab(u8fields_.size());
  for (std::size_t f = 0; f < u8fields_.size(); ++f) {
    lab[f] = u8fields_[f][child[0]];
  }
  uint8_t pmask = mask_[child[0]];
  for (int c = 1; c < 8; ++c) {
    if (mask_[child[static_cast<std::size_t>(c)]] != pmask) {
      pmask = static_cast<uint8_t>(CellMask::Cut);
      break;
    }
  }

  // Remove the 8 children (by key, re-finding each time) then add the parent.
  std::array<uint64_t, 8> childKeys{};
  for (int c = 0; c < 8; ++c) {
    childKeys[static_cast<std::size_t>(c)] = morton_[child[static_cast<std::size_t>(c)]];
  }
  for (const uint64_t k : childKeys) {
    swapRemove(map_.find(k)->second);
  }
  const std::size_t pi = appendCell(parentMorton, parentLevel, pmask);
  for (std::size_t f = 0; f < dfields_.size(); ++f) {
    dfields_[f][pi] = avg[f];
  }
  for (std::size_t f = 0; f < u8fields_.size(); ++f) {
    u8fields_[f][pi] = lab[f];
  }
  return true;
}

bool Octree::coarsenSiblings(std::size_t childIndex) {
  const int L = level_[childIndex];
  if (L == 0) {
    return false;
  }
  uint32_t ax, ay, az;
  anchorOf(childIndex, ax, ay, az);
  const uint32_t psz = cellSizeInt(L - 1);
  const uint32_t mask = ~(psz - 1u);
  const uint64_t pm = mortonEncode(ax & mask, ay & mask, az & mask);
  return coarsen(pm, L - 1);
}

// ---------------------------------------------------------------------------
// Gradient across level jumps
// ---------------------------------------------------------------------------
Vec3 Octree::gradient(const std::vector<double>& field, std::size_t i) const {
  Vec3 g;
  double* gp[3] = {&g.x, &g.y, &g.z};
  const double self = field[i];
  for (int axis = 0; axis < 3; ++axis) {
    const int dirMinus = 2 * axis;
    const int dirPlus = 2 * axis + 1;

    const auto side = [&](int dir, double& val, double& pos, bool& has) {
      const std::vector<std::size_t> nbs = faceNeighbors(i, dir);
      if (nbs.empty()) {
        has = false;
        return;
      }
      double sv = 0.0;
      double sp = 0.0;
      for (const std::size_t nb : nbs) {
        sv += field[nb];
        sp += centerAxis(nb, axis);
      }
      const double inv = 1.0 / static_cast<double>(nbs.size());
      val = sv * inv;
      pos = sp * inv;
      has = true;
    };

    double vP = self, pP = centerAxis(i, axis);
    double vM = self, pM = centerAxis(i, axis);
    bool hasP = false, hasM = false;
    side(dirPlus, vP, pP, hasP);
    side(dirMinus, vM, pM, hasM);

    if (!hasP && !hasM) {
      *gp[axis] = 0.0;
    } else {
      if (!hasP) {
        vP = self;
        pP = centerAxis(i, axis);
      }
      if (!hasM) {
        vM = self;
        pM = centerAxis(i, axis);
      }
      const double d = pP - pM;
      *gp[axis] = (d != 0.0) ? (vP - vM) / d : 0.0;
    }
  }
  return g;
}

// ---------------------------------------------------------------------------
// Invariants / serialization
// ---------------------------------------------------------------------------
bool Octree::isBalanced() const {
  // Each 2:1 violation has a finer cell whose coarser-or-equal neighbor is >1
  // level away, so scanning the coarser side of every cell catches all of them.
  for (std::size_t i = 0; i < cellCount(); ++i) {
    uint32_t ax, ay, az;
    anchorOf(i, ax, ay, az);
    const int L = level_[i];
    for (int dir = 0; dir < 6; ++dir) {
      int nlvl = 0;
      const std::size_t nb = coarserOrEqualFaceNeighbor(ax, ay, az, L, dir, nlvl);
      if (nb != npos && (L - nlvl) > 1) {
        return false;
      }
    }
  }
  return true;
}

OctreeState Octree::dumpState() const {
  OctreeState s;
  s.origin = origin_;
  s.extent = extent_;
  s.maxLevel = maxLevel_;
  s.morton = morton_;
  s.level = level_;
  s.mask = mask_;
  s.scalarNames = dnames_;
  s.scalars = dfields_;
  s.labelNames = u8names_;
  s.labels = u8fields_;
  return s;
}

}  // namespace nabla::mesh
