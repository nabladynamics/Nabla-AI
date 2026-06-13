#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nabla/mesh/morton.hpp"

// Linear adaptive octree with structure-of-arrays (SoA) field storage.
//
// Design goals (see docs/adr/0002-octree-from-scratch.md):
//   * Flat, contiguous, pointer-free storage — cache-friendly today and
//     portable to GPU buffers tomorrow.
//   * Leaves only. Each leaf is identified by its Morton-encoded anchor (lower
//     corner on the finest 2^maxLevel grid). That key is unique among leaves,
//     so an open hash map gives O(1) lookup with no tree pointers.
//   * Topology (parent/child/neighbor) is pure integer/bit arithmetic.
//
// The domain is a rectangular box [origin, origin+extent). Cells are axis-
// aligned boxes whose physical size per axis is extent_a / 2^level (anisotropic
// for a non-cubic domain — acceptable for Phase 0; a forest-of-octrees gives
// cubic cells later, see ADR-002).
namespace nabla::mesh {

enum class CellMask : uint8_t {
  Fluid = 0,        // entirely outside any solid
  Cut = 1,          // intersected by a solid surface
  InsideSolid = 2,  // entirely inside a solid
};

struct Vec3 {
  double x{0.0};
  double y{0.0};
  double z{0.0};
};

// Full serializable state — the unit of checkpoint I/O and a convenient handle
// for tests. Arrays are parallel (index = cell index).
struct OctreeState {
  Vec3 origin;
  Vec3 extent;
  int maxLevel{0};
  std::vector<uint64_t> morton;
  std::vector<uint8_t> level;
  std::vector<uint8_t> mask;
  std::vector<std::string> scalarNames;
  std::vector<std::vector<double>> scalars;
  std::vector<std::string> labelNames;
  std::vector<std::vector<uint8_t>> labels;
};

class Octree {
 public:
  static constexpr int kMaxSupportedLevel = kMortonBits;  // 21
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  // Creates a single root leaf (level 0) covering the whole domain. The core
  // velocity/pressure scalars (u, v, w, p) are pre-registered.
  Octree(Vec3 origin, Vec3 extent, int maxLevel);

  // Rebuild from serialized state (used by the checkpoint loader).
  explicit Octree(const OctreeState& state);

  // ---- topology / geometry ----------------------------------------------
  std::size_t cellCount() const noexcept { return level_.size(); }
  int maxLevel() const noexcept { return maxLevel_; }
  Vec3 origin() const noexcept { return origin_; }
  Vec3 extent() const noexcept { return extent_; }

  uint64_t morton(std::size_t i) const { return morton_[i]; }
  int level(std::size_t i) const { return level_[i]; }
  CellMask mask(std::size_t i) const { return static_cast<CellMask>(mask_[i]); }
  void setMask(std::size_t i, CellMask m) { mask_[i] = static_cast<uint8_t>(m); }

  Vec3 cellCenter(std::size_t i) const;
  Vec3 cellSize(std::size_t i) const;
  double cellVolume(std::size_t i) const;

  // Returns the cell index for a Morton key, or npos if absent.
  std::size_t find(uint64_t mortonKey) const;

  // ---- field registry (SoA) ---------------------------------------------
  int registerScalar(const std::string& name);  // double field; idempotent
  bool hasScalar(const std::string& name) const { return dindex_.count(name) != 0; }
  std::vector<double>& scalar(const std::string& name);
  const std::vector<double>& scalar(const std::string& name) const;
  std::vector<double>& scalar(int idx) { return dfields_[static_cast<std::size_t>(idx)]; }
  const std::vector<std::string>& scalarNames() const { return dnames_; }

  int registerLabel(const std::string& name);  // uint8 field; idempotent
  bool hasLabel(const std::string& name) const { return u8index_.count(name) != 0; }
  std::vector<uint8_t>& label(const std::string& name);
  const std::vector<uint8_t>& label(const std::string& name) const;
  const std::vector<std::string>& labelNames() const { return u8names_; }

  // Core fields, always present.
  std::vector<double>& u() { return dfields_[0]; }
  std::vector<double>& v() { return dfields_[1]; }
  std::vector<double>& w() { return dfields_[2]; }
  std::vector<double>& p() { return dfields_[3]; }

  // ---- refinement / coarsening ------------------------------------------
  // Split a leaf into 8 children with conservative (injection) interpolation.
  // Enforces 2:1 face balance automatically (refines coarser neighbors first).
  void refine(std::size_t cellIndex);
  void refineByMorton(uint64_t mortonKey);

  // Merge the 8 children of (parentMorton, parentLevel) using volume-weighted
  // averaging. Returns false (doing nothing) if the children are not all
  // present leaves, or if coarsening would break 2:1 balance.
  bool coarsen(uint64_t parentMorton, int parentLevel);
  bool coarsenSiblings(std::size_t childIndex);

  void refineUniform(int times);
  void refineBox(Vec3 lo, Vec3 hi, int times);  // refine leaves with center in box

  // ---- neighbors / stencils ---------------------------------------------
  // Face direction: 0=-x, 1=+x, 2=-y, 3=+y, 4=-z, 5=+z.
  std::vector<std::size_t> faceNeighbors(std::size_t i, int dir) const;
  // Cell-centered gradient using face neighbors; handles level jumps via the
  // physical distance between cell centers.
  Vec3 gradient(const std::vector<double>& field, std::size_t i) const;

  // ---- invariants / serialization ---------------------------------------
  bool isBalanced() const;  // true if all face-adjacent leaves differ by <= 1 level
  OctreeState dumpState() const;

 private:
  void anchorOf(std::size_t i, uint32_t& ax, uint32_t& ay, uint32_t& az) const {
    mortonDecode(morton_[i], ax, ay, az);
  }
  uint32_t cellSizeInt(int lvl) const { return 1u << (maxLevel_ - lvl); }
  double centerAxis(std::size_t i, int axis) const;

  std::size_t coarserOrEqualFaceNeighbor(uint32_t ax, uint32_t ay, uint32_t az,
                                         int lvl, int dir, int& outLevel) const;
  std::vector<std::size_t> faceNeighborsOf(uint32_t ax, uint32_t ay, uint32_t az,
                                           int lvl, int dir) const;

  std::size_t appendCell(uint64_t m, int lvl, uint8_t mk);
  void swapRemove(std::size_t i);
  void splitLeaf(std::size_t i);
  void rebuildIndex();

  Vec3 origin_;
  Vec3 extent_;
  int maxLevel_;
  double dxf_[3];  // finest cell size per axis = extent_a / 2^maxLevel

  // SoA topology
  std::vector<uint64_t> morton_;
  std::vector<uint8_t> level_;
  std::vector<uint8_t> mask_;

  // SoA double-field registry (slots 0..3 are u, v, w, p)
  std::vector<std::string> dnames_;
  std::unordered_map<std::string, int> dindex_;
  std::vector<std::vector<double>> dfields_;

  // SoA uint8-label registry (e.g., physics-model id)
  std::vector<std::string> u8names_;
  std::unordered_map<std::string, int> u8index_;
  std::vector<std::vector<uint8_t>> u8fields_;

  std::unordered_map<uint64_t, uint32_t> map_;  // anchor Morton -> cell index
};

}  // namespace nabla::mesh
