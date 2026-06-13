#include <algorithm>
#include <cstdint>
#include <vector>

#include "nabla/adaptive/adaptive.hpp"

namespace nabla::adaptive {

double octreeFieldIntegral(const Octree& tree, const std::string& name) {
  const auto& phi = tree.scalar(name);
  double s = 0.0;
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    s += phi[i] * tree.cellVolume(i);
  }
  return s;
}

// Conservative upwind finite-volume advection on the octree leaves, using the
// registered cell-centered velocity fields u,v,w. Domain boundaries are closed
// (no flux), and interior face fluxes are computed identically from both sides,
// so the total integral sum_i phi_i*vol_i is conserved to round-off — including
// across 2:1 level jumps. This is the "live solve" the AMR conservation test
// runs concurrently with refine/coarsen.
double octreeTransportStep(Octree& tree, const std::string& name, double dt) {
  const auto& U = tree.u();
  const auto& V = tree.v();
  const auto& W = tree.w();
  auto& phi = tree.scalar(name);
  const std::size_t n = tree.cellCount();

  std::vector<double> dphi(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const Vec3 szi = tree.cellSize(i);
    const double vol = tree.cellVolume(i);
    double netOut = 0.0;
    for (int dir = 0; dir < 6; ++dir) {
      const int axis = dir / 2;
      const double sign = (dir & 1) ? 1.0 : -1.0;  // +face vs -face normal
      const std::vector<std::size_t> nbs = tree.faceNeighbors(i, dir);
      // closed domain boundary => no flux (conserves the integral exactly).
      for (std::size_t nb : nbs) {
        const Vec3 sznb = tree.cellSize(nb);
        // shared face area = transverse area of the finer (smaller) cell.
        const double areaI = (axis == 0 ? szi.y * szi.z : axis == 1 ? szi.x * szi.z : szi.x * szi.y);
        const double areaN = (axis == 0 ? sznb.y * sznb.z : axis == 1 ? sznb.x * sznb.z : sznb.x * sznb.y);
        const double area = std::min(areaI, areaN);
        const double velI = (axis == 0 ? U[i] : axis == 1 ? V[i] : W[i]);
        const double velN = (axis == 0 ? U[nb] : axis == 1 ? V[nb] : W[nb]);
        const double vn = sign * 0.5 * (velI + velN);  // outward normal velocity
        const double upwind = (vn >= 0.0) ? phi[i] : phi[nb];
        netOut += vn * area * upwind;
      }
    }
    dphi[i] = -dt / vol * netOut;
  }
  for (std::size_t i = 0; i < n; ++i) {
    phi[i] += dphi[i];
  }
  return octreeFieldIntegral(tree, name);
}

}  // namespace nabla::adaptive
