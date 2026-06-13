#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nabla::flow {

struct V3 {
  double x{0.0};
  double y{0.0};
  double z{0.0};
};

// Boundary kinds on each of the six domain faces. A "Wall" carries a prescribed
// velocity vector: zero for a no-slip wall, a tangential value for a moving lid.
enum class BoundaryType { Wall, Inlet, Outlet, Periodic };
enum class InletProfile { Uniform, Parabolic };

struct FaceBC {
  BoundaryType type = BoundaryType::Wall;
  V3 wallVelocity{0.0, 0.0, 0.0};  // for Wall (no-slip = 0, moving lid != 0)
  InletProfile profile = InletProfile::Uniform;
  double inletSpeed = 0.0;  // bulk speed driving the inlet
  int inletAxis = 0;        // velocity component the inlet drives (0=x,1=y,2=z)
};

// Uniform Cartesian mesh (cell-centered) with a halo of width kHalo so wide WENO
// stencils need no per-face branching. Interior indices run [0, n); ghost layers
// occupy [-kHalo, 0) and [n, n+kHalo). Face order matches the octree: 0=-x, 1=+x,
// 2=-y, 3=+y, 4=-z, 5=+z. nz==1 with periodic z gives a clean 2D problem.
struct UniformGrid {
  int nx = 1, ny = 1, nz = 1;
  V3 origin{0.0, 0.0, 0.0};
  double dx = 1.0, dy = 1.0, dz = 1.0;

  FaceBC face[6];
  std::vector<uint8_t> solid;  // interior-cell solid flag (1 = no-slip obstacle)

  void allocate() { solid.assign(cellCount(), 0); }

  [[nodiscard]] std::size_t cellCount() const {
    return static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) *
           static_cast<std::size_t>(nz);
  }

  // Index into a compact (no-halo) interior array.
  [[nodiscard]] std::size_t cidx(int i, int j, int k) const {
    return static_cast<std::size_t>(i) +
           static_cast<std::size_t>(nx) *
               (static_cast<std::size_t>(j) + static_cast<std::size_t>(ny) *
                                                  static_cast<std::size_t>(k));
  }

  [[nodiscard]] bool isSolid(int i, int j, int k) const {
    if (i < 0 || i >= nx || j < 0 || j >= ny || k < 0 || k >= nz) {
      return false;
    }
    return solid[cidx(i, j, k)] != 0;
  }

  [[nodiscard]] V3 cellCenter(int i, int j, int k) const {
    return {origin.x + (static_cast<double>(i) + 0.5) * dx,
            origin.y + (static_cast<double>(j) + 0.5) * dy,
            origin.z + (static_cast<double>(k) + 0.5) * dz};
  }
  [[nodiscard]] double cellVolume() const { return dx * dy * dz; }

  [[nodiscard]] bool periodic(int axis) const {
    const int lo = 2 * axis;
    return face[lo].type == BoundaryType::Periodic;
  }
};

}  // namespace nabla::flow
