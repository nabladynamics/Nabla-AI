// nabla_mesh_demo — builds an adaptive octree over a rectangular CFD-style
// domain, refines a box around the origin by 3 levels, fills some fields, and
// writes a ParaView-openable .vtu (and an HDF5 checkpoint when available).
//
//   Usage: nabla_mesh_demo [out.vtu] [checkpoint.h5]

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "nabla/io/checkpoint.hpp"
#include "nabla/io/vtu_writer.hpp"
#include "nabla/mesh/octree.hpp"

int main(int argc, char** argv) {
  using namespace nabla::mesh;

  const std::string vtuPath = (argc > 1) ? argv[1] : "nabla_mesh_demo.vtu";
  const std::string ckptPath = (argc > 2) ? argv[2] : "nabla_mesh_demo.h5";

  // Reference length h; domain 14h x 3h x 6.4h centered on the origin so the
  // origin is interior (refinement box sits around it).
  const double h = 1.0;
  const Vec3 extent{14.0 * h, 3.0 * h, 6.4 * h};
  const Vec3 origin{-0.5 * extent.x, -0.5 * extent.y, -0.5 * extent.z};
  const int maxLevel = 12;  // capacity (12 levels); the demo only uses ~5

  Octree tree(origin, extent, maxLevel);

  // Extensible per-cell registry beyond the core u,v,w,p.
  tree.registerScalar("tke");
  tree.registerScalar("dissipation");
  tree.registerScalar("refine_indicator");
  tree.registerLabel("physics_model");

  // Uniform base mesh, then 3 extra levels in a box hugging the origin.
  tree.refineUniform(2);
  const Vec3 boxLo{-2.0 * h, -0.6 * h, -1.2 * h};
  const Vec3 boxHi{2.0 * h, 0.6 * h, 1.2 * h};
  tree.refineBox(boxLo, boxHi, 3);

  // Fill fields with a smooth analytic pattern + a region-based model label.
  std::vector<double>& u = tree.u();
  std::vector<double>& v = tree.v();
  std::vector<double>& w = tree.w();
  std::vector<double>& p = tree.p();
  std::vector<double>& tke = tree.scalar("tke");
  std::vector<uint8_t>& model = tree.label("physics_model");

  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    const Vec3 c = tree.cellCenter(i);
    u[i] = std::sin(c.x / h);
    v[i] = std::cos(c.y / h);
    w[i] = 0.1 * c.z / h;
    p[i] = -0.5 * (u[i] * u[i] + v[i] * v[i]);
    tke[i] = 0.5 * (u[i] * u[i] + v[i] * v[i] + w[i] * w[i]);
    // Pretend the refined near-origin box runs a finer turbulence model (id 1).
    const bool inBox = c.x >= boxLo.x && c.x <= boxHi.x && c.y >= boxLo.y &&
                       c.y <= boxHi.y && c.z >= boxLo.z && c.z <= boxHi.z;
    model[i] = inBox ? uint8_t{1} : uint8_t{0};
  }

  int minL = maxLevel, maxL = 0;
  for (std::size_t i = 0; i < tree.cellCount(); ++i) {
    minL = std::min(minL, tree.level(i));
    maxL = std::max(maxL, tree.level(i));
  }

  std::cout << "Nabla AI octree demo\n"
            << "  domain      : " << extent.x << " x " << extent.y << " x " << extent.z
            << " (origin " << origin.x << ", " << origin.y << ", " << origin.z << ")\n"
            << "  cells       : " << tree.cellCount() << '\n'
            << "  levels      : " << minL << " .. " << maxL << '\n'
            << "  2:1 balanced: " << (tree.isBalanced() ? "yes" : "NO") << '\n';

  nabla::io::writeVtu(tree, vtuPath);
  std::cout << "  wrote VTU   : " << vtuPath << "  (open in ParaView)\n";

  if (nabla::io::hdf5Available()) {
    nabla::io::writeCheckpoint(tree, ckptPath);
    std::cout << "  wrote ckpt  : " << ckptPath << '\n';
  } else {
    std::cout << "  (HDF5 checkpoint skipped — built without HDF5)\n";
  }

  return tree.isBalanced() ? 0 : 1;
}
