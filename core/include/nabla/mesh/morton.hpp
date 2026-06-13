#pragma once

#include <cstdint>

// 3D Morton (Z-order) encoding for octree cell IDs.
//
// We interleave 21 bits per axis into a 63-bit code that fits in a uint64_t.
// 21 levels of refinement is far beyond the Phase-0 target of 10+; it is the
// natural ceiling for a single 64-bit key (3 * 21 = 63).
//
// Header-only and branch-free (magic-number bit spreading) so it inlines into
// the hot refine/coarsen/neighbor paths with no function-call overhead.
namespace nabla::mesh {

inline constexpr int kMortonBits = 21;                 // bits per axis
inline constexpr uint32_t kMortonMaxCoord = (1u << kMortonBits) - 1u;

// Spread the low 21 bits of `a` so that bit i lands at bit 3*i.
inline uint64_t mortonSplit3(uint32_t a) noexcept {
  uint64_t x = a & 0x1fffffULL;                        // keep 21 bits
  x = (x | (x << 32)) & 0x1f00000000ffffULL;
  x = (x | (x << 16)) & 0x1f0000ff0000ffULL;
  x = (x | (x << 8)) & 0x100f00f00f00f00fULL;
  x = (x | (x << 4)) & 0x10c30c30c30c30c3ULL;
  x = (x | (x << 2)) & 0x1249249249249249ULL;
  return x;
}

// Inverse of mortonSplit3: gather bits at 3*i back into the low 21 bits.
inline uint32_t mortonCompact3(uint64_t x) noexcept {
  x &= 0x1249249249249249ULL;
  x = (x ^ (x >> 2)) & 0x10c30c30c30c30c3ULL;
  x = (x ^ (x >> 4)) & 0x100f00f00f00f00fULL;
  x = (x ^ (x >> 8)) & 0x1f0000ff0000ffULL;
  x = (x ^ (x >> 16)) & 0x1f00000000ffffULL;
  x = (x ^ (x >> 32)) & 0x1fffffULL;
  return static_cast<uint32_t>(x);
}

inline uint64_t mortonEncode(uint32_t x, uint32_t y, uint32_t z) noexcept {
  return mortonSplit3(x) | (mortonSplit3(y) << 1) | (mortonSplit3(z) << 2);
}

inline void mortonDecode(uint64_t code, uint32_t& x, uint32_t& y, uint32_t& z) noexcept {
  x = mortonCompact3(code);
  y = mortonCompact3(code >> 1);
  z = mortonCompact3(code >> 2);
}

}  // namespace nabla::mesh
