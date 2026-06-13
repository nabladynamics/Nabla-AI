#include "nabla/flow/ns_solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace nabla::flow {
namespace {

// WENO-5 (Jiang–Shu) reconstruction at the right face of the middle cell, from a
// left-biased 5-cell stencil {v0,v1,v2,v3,v4} = cells (i-2..i+2). Upwind for
// positive advection; the negative case calls this with the mirrored stencil.
double weno5(double v0, double v1, double v2, double v3, double v4) {
  constexpr double eps = 1e-12;
  const double b0 = 13.0 / 12.0 * (v0 - 2 * v1 + v2) * (v0 - 2 * v1 + v2) +
                    0.25 * (v0 - 4 * v1 + 3 * v2) * (v0 - 4 * v1 + 3 * v2);
  const double b1 = 13.0 / 12.0 * (v1 - 2 * v2 + v3) * (v1 - 2 * v2 + v3) +
                    0.25 * (v1 - v3) * (v1 - v3);
  const double b2 = 13.0 / 12.0 * (v2 - 2 * v3 + v4) * (v2 - 2 * v3 + v4) +
                    0.25 * (3 * v2 - 4 * v3 + v4) * (3 * v2 - 4 * v3 + v4);
  const double a0 = 0.1 / ((eps + b0) * (eps + b0));
  const double a1 = 0.6 / ((eps + b1) * (eps + b1));
  const double a2 = 0.3 / ((eps + b2) * (eps + b2));
  const double s = a0 + a1 + a2;
  const double p0 = (2 * v0 - 7 * v1 + 11 * v2) / 6.0;
  const double p1 = (-v1 + 5 * v2 + 2 * v3) / 6.0;
  const double p2 = (2 * v2 + 5 * v3 - v4) / 6.0;
  return (a0 * p0 + a1 * p1 + a2 * p2) / s;
}

// Reconstruct the advected quantity at a flux face from the 6 cells straddling
// it (qm2..qp3 = cells i-2..i+3, face between i and i+1), upwinded by `adv`.
double reconstruct(ConvectionScheme scheme, double adv, double qm2, double qm1,
                   double q0, double qp1, double qp2, double qp3) {
  if (scheme == ConvectionScheme::Central) {
    return 0.5 * (q0 + qp1);
  }
  return (adv >= 0.0) ? weno5(qm2, qm1, q0, qp1, qp2)
                      : weno5(qp3, qp2, qp1, q0, qm1);
}

}  // namespace

NSSolver::NSSolver(UniformGrid grid, FlowConfig config,
                   std::shared_ptr<PoissonSolver> poisson)
    : grid_(std::move(grid)), cfg_(config), poisson_(std::move(poisson)) {
  if (!poisson_) {
    // Geometric multigrid (MG-PCG) is the default pressure solver — O(N) and
    // mesh-change friendly (ADR-0007). poissonMaxIters caps the MG-PCG cycles.
    poisson_ = std::make_shared<MgPoisson>(cfg_.poissonTol, cfg_.poissonMaxIters);
  }
  nu_ = cfg_.velocityScale * cfg_.refLength / cfg_.reynolds;
  if (grid_.solid.empty()) {
    grid_.allocate();
  }
  const auto sz = [](int a) { return static_cast<std::size_t>(a); };
  u_.assign(sz(grid_.nx + 1) * sz(grid_.ny) * sz(grid_.nz), 0.0);
  v_.assign(sz(grid_.nx) * sz(grid_.ny + 1) * sz(grid_.nz), 0.0);
  w_.assign(sz(grid_.nx) * sz(grid_.ny) * sz(grid_.nz + 1), 0.0);
  p_.assign(grid_.cellCount(), 0.0);
  for (uint8_t s : grid_.solid) {
    if (s) {
      hasSolid_ = true;
      break;
    }
  }
}

V3 NSSolver::inletVelocityAt(int faceId, int j, int k) const {
  const FaceBC& fb = grid_.face[faceId];
  double mag = fb.inletSpeed;
  if (fb.profile == InletProfile::Parabolic) {
    // Parabolic in the wall-normal direction (assume walls bound y for a channel
    // when the inlet is on x; otherwise use y as the profile coordinate).
    const double H = grid_.ny * grid_.dy;
    const double yc = grid_.origin.y + 0.5 * H;
    const double y = grid_.origin.y + (j + 0.5) * grid_.dy;
    const double r = (y - yc) / (0.5 * H);
    mag = 1.5 * fb.inletSpeed * (1.0 - r * r);
    (void)k;
  }
  V3 vel{0, 0, 0};
  if (fb.inletAxis == 0) {
    vel.x = mag;
  } else if (fb.inletAxis == 1) {
    vel.y = mag;
  } else {
    vel.z = mag;
  }
  return vel;
}

void NSSolver::initialize() {
  std::fill(u_.begin(), u_.end(), 0.0);
  std::fill(v_.begin(), v_.end(), 0.0);
  std::fill(w_.begin(), w_.end(), 0.0);
  std::fill(p_.begin(), p_.end(), 0.0);
  // Seed interior with the inlet bulk speed (helps channel-type cases converge).
  for (int f = 0; f < 6; ++f) {
    if (grid_.face[f].type == BoundaryType::Inlet && grid_.face[f].inletAxis == 0) {
      const double s = grid_.face[f].inletSpeed;
      std::fill(u_.begin(), u_.end(), s);
      break;
    }
  }
  applyVelocityBC(u_, v_, w_);
  step_ = 0;
  t_ = 0.0;
  dtLast_ = 0.0;
}

// --- BC-aware staggered accessors -----------------------------------------
// Only one index is ever out of range for our axis-separated stencils.
double NSSolver::U(const std::vector<double>& u, int i, int j, int k) const {
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  if (k < 0 || k >= nz) {
    if (grid_.periodic(2)) {
      return U(u, i, j, (k % nz + nz) % nz);
    }
    const int km = (k < 0) ? (-1 - k) : (2 * nz - 1 - k);
    const int face = (k < 0) ? 4 : 5;
    return 2.0 * grid_.face[face].wallVelocity.x - U(u, i, j, km);
  }
  if (j < 0 || j >= ny) {
    if (grid_.periodic(1)) {
      return U(u, i, (j % ny + ny) % ny, k);
    }
    const int jm = (j < 0) ? (-1 - j) : (2 * ny - 1 - j);
    const int face = (j < 0) ? 2 : 3;
    return 2.0 * grid_.face[face].wallVelocity.x - U(u, i, jm, k);
  }
  if (i < 0 || i > nx) {
    if (grid_.periodic(0)) {
      return U(u, (i % nx + nx) % nx, j, k);
    }
    // reflect about the boundary x-face value (i=0 or i=nx), which is stored.
    const int ib = (i < 0) ? 0 : nx;
    const int ir = 2 * ib - i;
    const int face = (i < 0) ? 0 : 1;
    if (grid_.face[face].type == BoundaryType::Outlet) {
      return u[ui(std::clamp(ib, 0, nx), j, k)];  // zero-gradient
    }
    return 2.0 * u[ui(ib, j, k)] - U(u, ir, j, k);
  }
  return u[ui(i, j, k)];
}

double NSSolver::Vv(const std::vector<double>& v, int i, int j, int k) const {
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  if (k < 0 || k >= nz) {
    if (grid_.periodic(2)) {
      return Vv(v, i, j, (k % nz + nz) % nz);
    }
    const int km = (k < 0) ? (-1 - k) : (2 * nz - 1 - k);
    const int face = (k < 0) ? 4 : 5;
    return 2.0 * grid_.face[face].wallVelocity.y - Vv(v, i, j, km);
  }
  if (i < 0 || i >= nx) {
    if (grid_.periodic(0)) {
      return Vv(v, (i % nx + nx) % nx, j, k);
    }
    const int im = (i < 0) ? (-1 - i) : (2 * nx - 1 - i);
    const int face = (i < 0) ? 0 : 1;
    if (grid_.face[face].type == BoundaryType::Inlet) {
      return 2.0 * inletVelocityAt(face, j, k).y - Vv(v, im, j, k);  // ~0
    }
    if (grid_.face[face].type == BoundaryType::Outlet) {
      return v[vi(std::clamp(i, 0, nx - 1), j, k)];
    }
    return 2.0 * grid_.face[face].wallVelocity.y - Vv(v, im, j, k);
  }
  if (j < 0 || j > ny) {
    if (grid_.periodic(1)) {
      return Vv(v, i, (j % ny + ny) % ny, k);
    }
    const int jb = (j < 0) ? 0 : ny;
    const int jr = 2 * jb - j;
    const int face = (j < 0) ? 2 : 3;
    if (grid_.face[face].type == BoundaryType::Outlet) {
      return v[vi(i, std::clamp(jb, 0, ny), k)];
    }
    return 2.0 * v[vi(i, jb, k)] - Vv(v, i, jr, k);
  }
  return v[vi(i, j, k)];
}

double NSSolver::W(const std::vector<double>& w, int i, int j, int k) const {
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  if (j < 0 || j >= ny) {
    if (grid_.periodic(1)) {
      return W(w, i, (j % ny + ny) % ny, k);
    }
    const int jm = (j < 0) ? (-1 - j) : (2 * ny - 1 - j);
    const int face = (j < 0) ? 2 : 3;
    return 2.0 * grid_.face[face].wallVelocity.z - W(w, i, jm, k);
  }
  if (i < 0 || i >= nx) {
    if (grid_.periodic(0)) {
      return W(w, (i % nx + nx) % nx, j, k);
    }
    const int im = (i < 0) ? (-1 - i) : (2 * nx - 1 - i);
    const int face = (i < 0) ? 0 : 1;
    if (grid_.face[face].type == BoundaryType::Outlet) {
      return w[wi(std::clamp(i, 0, nx - 1), j, k)];
    }
    return 2.0 * grid_.face[face].wallVelocity.z - W(w, im, j, k);
  }
  if (k < 0 || k > nz) {
    if (grid_.periodic(2)) {
      return W(w, i, j, (k % nz + nz) % nz);
    }
    const int kb = (k < 0) ? 0 : nz;
    const int kr = 2 * kb - k;
    return 2.0 * w[wi(i, j, kb)] - W(w, i, j, kr);
  }
  return w[wi(i, j, k)];
}

void NSSolver::applyVelocityBC(std::vector<double>& u, std::vector<double>& v,
                               std::vector<double>& w, bool enforceOutflow) const {
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  // x-normal boundaries (u at i=0, i=nx)
  for (int k = 0; k < nz; ++k) {
    for (int j = 0; j < ny; ++j) {
      for (int side = 0; side < 2; ++side) {
        const int face = side;  // 0=-x,1=+x
        const int i = side == 0 ? 0 : nx;
        switch (grid_.face[face].type) {
          case BoundaryType::Wall: u[ui(i, j, k)] = 0.0; break;
          case BoundaryType::Inlet: u[ui(i, j, k)] = inletVelocityAt(face, j, k).x; break;
          case BoundaryType::Outlet:
            if (enforceOutflow) u[ui(i, j, k)] = u[ui(side == 0 ? 1 : nx - 1, j, k)];
            break;
          case BoundaryType::Periodic: break;
        }
      }
    }
  }
  // y-normal boundaries (v at j=0, j=ny)
  for (int k = 0; k < nz; ++k) {
    for (int i = 0; i < nx; ++i) {
      for (int side = 0; side < 2; ++side) {
        const int face = 2 + side;
        const int j = side == 0 ? 0 : ny;
        switch (grid_.face[face].type) {
          case BoundaryType::Wall: v[vi(i, j, k)] = 0.0; break;
          case BoundaryType::Inlet: v[vi(i, j, k)] = inletVelocityAt(face, i, k).y; break;
          case BoundaryType::Outlet:
            if (enforceOutflow) v[vi(i, j, k)] = v[vi(i, side == 0 ? 1 : ny - 1, k)];
            break;
          case BoundaryType::Periodic: break;
        }
      }
    }
  }
  // z-normal boundaries (w at k=0, k=nz)
  if (nz > 1) {
    for (int j = 0; j < ny; ++j) {
      for (int i = 0; i < nx; ++i) {
        for (int side = 0; side < 2; ++side) {
          const int face = 4 + side;
          const int k = side == 0 ? 0 : nz;
          switch (grid_.face[face].type) {
            case BoundaryType::Wall: w[wi(i, j, k)] = 0.0; break;
            case BoundaryType::Inlet: w[wi(i, j, k)] = inletVelocityAt(face, i, j).z; break;
            case BoundaryType::Outlet:
              if (enforceOutflow) w[wi(i, j, k)] = w[wi(i, j, side == 0 ? 1 : nz - 1)];
              break;
            case BoundaryType::Periodic: break;
          }
        }
      }
    }
  }
  // Solid (no-slip immersed body): zero every face touching a solid cell.
  if (hasSolid_) {
    for (int k = 0; k < nz; ++k) {
      for (int j = 0; j < ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
          if (grid_.isSolid(i - 1, j, k) || grid_.isSolid(i, j, k)) {
            u[ui(i, j, k)] = 0.0;
          }
        }
      }
    }
    for (int k = 0; k < nz; ++k) {
      for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i < nx; ++i) {
          if (grid_.isSolid(i, j - 1, k) || grid_.isSolid(i, j, k)) {
            v[vi(i, j, k)] = 0.0;
          }
        }
      }
    }
    if (nz > 1) {
      for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j < ny; ++j) {
          for (int i = 0; i < nx; ++i) {
            if (grid_.isSolid(i, j, k - 1) || grid_.isSolid(i, j, k)) {
              w[wi(i, j, k)] = 0.0;
            }
          }
        }
      }
    }
  }
}

void NSSolver::computeRhs(const std::vector<double>& u, const std::vector<double>& v,
                          const std::vector<double>& w, std::vector<double>& ru,
                          std::vector<double>& rv, std::vector<double>& rw) const {
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  const double dx = grid_.dx, dy = grid_.dy, dz = grid_.dz;
  const bool twoD = (nz == 1);
  const ConvectionScheme sc = cfg_.convection;
  ru.assign(u.size(), 0.0);
  rv.assign(v.size(), 0.0);
  rw.assign(w.size(), 0.0);

  // ---- u-momentum (x-faces) ----
  for (int k = 0; k < nz; ++k) {
    for (int j = 0; j < ny; ++j) {
      for (int i = 1; i < nx; ++i) {  // interior x-faces
        if (grid_.isSolid(i - 1, j, k) || grid_.isSolid(i, j, k)) {
          continue;
        }
        // convection (divergence form): d(uu)/dx + d(uv)/dy + d(uw)/dz
        // F^x = u*u at cell centers i-1 and i (advect = uc)
        const double ucL = 0.5 * (U(u, i - 1, j, k) + U(u, i, j, k));
        const double ucR = 0.5 * (U(u, i, j, k) + U(u, i + 1, j, k));
        const double uxL = reconstruct(sc, ucL, U(u, i - 3, j, k), U(u, i - 2, j, k),
                                       U(u, i - 1, j, k), U(u, i, j, k), U(u, i + 1, j, k),
                                       U(u, i + 2, j, k));
        const double uxR = reconstruct(sc, ucR, U(u, i - 2, j, k), U(u, i - 1, j, k),
                                       U(u, i, j, k), U(u, i + 1, j, k), U(u, i + 2, j, k),
                                       U(u, i + 3, j, k));
        const double dFx = (ucR * uxR - ucL * uxL) / dx;

        // F^y = u*v at corners (i,j+1/2) and (i,j-1/2)
        const double vT = 0.5 * (Vv(v, i - 1, j + 1, k) + Vv(v, i, j + 1, k));
        const double vB = 0.5 * (Vv(v, i - 1, j, k) + Vv(v, i, j, k));
        const double uyT = reconstruct(sc, vT, U(u, i, j - 2, k), U(u, i, j - 1, k),
                                       U(u, i, j, k), U(u, i, j + 1, k), U(u, i, j + 2, k),
                                       U(u, i, j + 3, k));
        const double uyB = reconstruct(sc, vB, U(u, i, j - 3, k), U(u, i, j - 2, k),
                                       U(u, i, j - 1, k), U(u, i, j, k), U(u, i, j + 1, k),
                                       U(u, i, j + 2, k));
        const double dFy = (vT * uyT - vB * uyB) / dy;

        double dFz = 0.0;
        if (!twoD) {
          const double wT = 0.5 * (W(w, i - 1, j, k + 1) + W(w, i, j, k + 1));
          const double wB = 0.5 * (W(w, i - 1, j, k) + W(w, i, j, k));
          const double uzT = reconstruct(sc, wT, U(u, i, j, k - 2), U(u, i, j, k - 1),
                                         U(u, i, j, k), U(u, i, j, k + 1),
                                         U(u, i, j, k + 2), U(u, i, j, k + 3));
          const double uzB = reconstruct(sc, wB, U(u, i, j, k - 3), U(u, i, j, k - 2),
                                         U(u, i, j, k - 1), U(u, i, j, k),
                                         U(u, i, j, k + 1), U(u, i, j, k + 2));
          dFz = (wT * uzT - wB * uzB) / dz;
        }

        // diffusion (central)
        double lap = (U(u, i + 1, j, k) - 2 * U(u, i, j, k) + U(u, i - 1, j, k)) / (dx * dx) +
                     (U(u, i, j + 1, k) - 2 * U(u, i, j, k) + U(u, i, j - 1, k)) / (dy * dy);
        if (!twoD) {
          lap += (U(u, i, j, k + 1) - 2 * U(u, i, j, k) + U(u, i, j, k - 1)) / (dz * dz);
        }
        ru[ui(i, j, k)] = -(dFx + dFy + dFz) + nu_ * lap + cfg_.bodyForce.x;
      }
    }
  }

  // ---- v-momentum (y-faces) ----
  for (int k = 0; k < nz; ++k) {
    for (int j = 1; j < ny; ++j) {
      for (int i = 0; i < nx; ++i) {
        if (grid_.isSolid(i, j - 1, k) || grid_.isSolid(i, j, k)) {
          continue;
        }
        const double vcB = 0.5 * (Vv(v, i, j - 1, k) + Vv(v, i, j, k));
        const double vcT = 0.5 * (Vv(v, i, j, k) + Vv(v, i, j + 1, k));
        const double vyB = reconstruct(sc, vcB, Vv(v, i, j - 3, k), Vv(v, i, j - 2, k),
                                       Vv(v, i, j - 1, k), Vv(v, i, j, k), Vv(v, i, j + 1, k),
                                       Vv(v, i, j + 2, k));
        const double vyT = reconstruct(sc, vcT, Vv(v, i, j - 2, k), Vv(v, i, j - 1, k),
                                       Vv(v, i, j, k), Vv(v, i, j + 1, k), Vv(v, i, j + 2, k),
                                       Vv(v, i, j + 3, k));
        const double dFy = (vcT * vyT - vcB * vyB) / dy;

        const double uR = 0.5 * (U(u, i + 1, j - 1, k) + U(u, i + 1, j, k));
        const double uL = 0.5 * (U(u, i, j - 1, k) + U(u, i, j, k));
        const double vxR = reconstruct(sc, uR, Vv(v, i - 2, j, k), Vv(v, i - 1, j, k),
                                       Vv(v, i, j, k), Vv(v, i + 1, j, k), Vv(v, i + 2, j, k),
                                       Vv(v, i + 3, j, k));
        const double vxL = reconstruct(sc, uL, Vv(v, i - 3, j, k), Vv(v, i - 2, j, k),
                                       Vv(v, i - 1, j, k), Vv(v, i, j, k), Vv(v, i + 1, j, k),
                                       Vv(v, i + 2, j, k));
        const double dFx = (uR * vxR - uL * vxL) / dx;

        double dFz = 0.0;
        if (!twoD) {
          const double wR = 0.5 * (W(w, i, j - 1, k + 1) + W(w, i, j, k + 1));
          const double wL = 0.5 * (W(w, i, j - 1, k) + W(w, i, j, k));
          const double vzR = reconstruct(sc, wR, Vv(v, i, j, k - 2), Vv(v, i, j, k - 1),
                                         Vv(v, i, j, k), Vv(v, i, j, k + 1),
                                         Vv(v, i, j, k + 2), Vv(v, i, j, k + 3));
          const double vzL = reconstruct(sc, wL, Vv(v, i, j, k - 3), Vv(v, i, j, k - 2),
                                         Vv(v, i, j, k - 1), Vv(v, i, j, k),
                                         Vv(v, i, j, k + 1), Vv(v, i, j, k + 2));
          dFz = (wR * vzR - wL * vzL) / dz;
        }

        double lap = (Vv(v, i + 1, j, k) - 2 * Vv(v, i, j, k) + Vv(v, i - 1, j, k)) / (dx * dx) +
                     (Vv(v, i, j + 1, k) - 2 * Vv(v, i, j, k) + Vv(v, i, j - 1, k)) / (dy * dy);
        if (!twoD) {
          lap += (Vv(v, i, j, k + 1) - 2 * Vv(v, i, j, k) + Vv(v, i, j, k - 1)) / (dz * dz);
        }
        rv[vi(i, j, k)] = -(dFx + dFy + dFz) + nu_ * lap + cfg_.bodyForce.y;
      }
    }
  }

  // ---- w-momentum (z-faces), only in 3D ----
  if (!twoD) {
    for (int k = 1; k < nz; ++k) {
      for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
          if (grid_.isSolid(i, j, k - 1) || grid_.isSolid(i, j, k)) {
            continue;
          }
          const double wcB = 0.5 * (W(w, i, j, k - 1) + W(w, i, j, k));
          const double wcT = 0.5 * (W(w, i, j, k) + W(w, i, j, k + 1));
          const double wzB = reconstruct(sc, wcB, W(w, i, j, k - 3), W(w, i, j, k - 2),
                                         W(w, i, j, k - 1), W(w, i, j, k), W(w, i, j, k + 1),
                                         W(w, i, j, k + 2));
          const double wzT = reconstruct(sc, wcT, W(w, i, j, k - 2), W(w, i, j, k - 1),
                                         W(w, i, j, k), W(w, i, j, k + 1), W(w, i, j, k + 2),
                                         W(w, i, j, k + 3));
          const double dFz = (wcT * wzT - wcB * wzB) / dz;

          const double uR = 0.5 * (U(u, i + 1, j, k - 1) + U(u, i + 1, j, k));
          const double uL = 0.5 * (U(u, i, j, k - 1) + U(u, i, j, k));
          const double wxR = reconstruct(sc, uR, W(w, i - 2, j, k), W(w, i - 1, j, k),
                                         W(w, i, j, k), W(w, i + 1, j, k), W(w, i + 2, j, k),
                                         W(w, i + 3, j, k));
          const double wxL = reconstruct(sc, uL, W(w, i - 3, j, k), W(w, i - 2, j, k),
                                         W(w, i - 1, j, k), W(w, i, j, k), W(w, i + 1, j, k),
                                         W(w, i + 2, j, k));
          const double dFx = (uR * wxR - uL * wxL) / dx;

          const double vR = 0.5 * (Vv(v, i, j + 1, k - 1) + Vv(v, i, j + 1, k));
          const double vL = 0.5 * (Vv(v, i, j, k - 1) + Vv(v, i, j, k));
          const double wyR = reconstruct(sc, vR, W(w, i, j - 2, k), W(w, i, j - 1, k),
                                         W(w, i, j, k), W(w, i, j + 1, k), W(w, i, j + 2, k),
                                         W(w, i, j + 3, k));
          const double wyL = reconstruct(sc, vL, W(w, i, j - 3, k), W(w, i, j - 2, k),
                                         W(w, i, j - 1, k), W(w, i, j, k), W(w, i, j + 1, k),
                                         W(w, i, j + 2, k));
          const double dFy = (vR * wyR - vL * wyL) / dy;

          const double lap =
              (W(w, i + 1, j, k) - 2 * W(w, i, j, k) + W(w, i - 1, j, k)) / (dx * dx) +
              (W(w, i, j + 1, k) - 2 * W(w, i, j, k) + W(w, i, j - 1, k)) / (dy * dy) +
              (W(w, i, j, k + 1) - 2 * W(w, i, j, k) + W(w, i, j, k - 1)) / (dz * dz);
          rw[wi(i, j, k)] = -(dFx + dFy + dFz) + nu_ * lap + cfg_.bodyForce.z;
        }
      }
    }
  }
}

void NSSolver::project(std::vector<double>& u, std::vector<double>& v,
                       std::vector<double>& w, double dt, int& poissonIters,
                       double& poissonRes) {
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  const double dx = grid_.dx, dy = grid_.dy, dz = grid_.dz;
  applyVelocityBC(u, v, w);

  // RHS b = -div(u*) (compact, exact on the staggered grid).
  std::vector<double> b(grid_.cellCount(), 0.0);
  for (int k = 0; k < nz; ++k) {
    for (int j = 0; j < ny; ++j) {
      for (int i = 0; i < nx; ++i) {
        if (grid_.isSolid(i, j, k)) {
          continue;
        }
        double div = (u[ui(i + 1, j, k)] - u[ui(i, j, k)]) / dx +
                     (v[vi(i, j + 1, k)] - v[vi(i, j, k)]) / dy;
        if (nz > 1) {
          div += (w[wi(i, j, k + 1)] - w[wi(i, j, k)]) / dz;
        }
        b[ci(i, j, k)] = -div;
      }
    }
  }

  std::vector<double> phi;
  const PoissonSolver::Report rep = poisson_->solve(grid_, b, phi);
  poissonIters = rep.iterations;
  poissonRes = rep.residual;

  // phi value at a cell with BC ghosts (Neumann at wall/inlet, Dirichlet 0 at
  // outlet, periodic wrap). Solid cells return their stored (0) — never used for
  // a face we correct, since we skip solid-touching faces.
  const auto phiAt = [&](int i, int j, int k) -> double {
    auto resolve = [&](int& a, int n, int axis) -> double {
      if (a >= 0 && a < n) {
        return 1.0;  // in range
      }
      if (grid_.periodic(axis)) {
        a = (a % n + n) % n;
        return 1.0;
      }
      const int faceId = 2 * axis + (a < 0 ? 0 : 1);
      a = std::clamp(a, 0, n - 1);
      return grid_.face[faceId].type == BoundaryType::Outlet ? -1.0 : 1.0;
    };
    double sgn = 1.0;
    sgn *= resolve(i, nx, 0);
    sgn *= resolve(j, ny, 1);
    sgn *= resolve(k, nz, 2);
    return sgn * phi[ci(i, j, k)];
  };

  // Correct interior + outlet faces by the compact pressure gradient. Faces that
  // touch a wall/inlet (Neumann) get a zero correction automatically; solid-
  // touching faces are skipped.
  for (int k = 0; k < nz; ++k) {
    for (int j = 0; j < ny; ++j) {
      for (int i = 0; i <= nx; ++i) {
        if (grid_.isSolid(i - 1, j, k) || grid_.isSolid(i, j, k)) {
          continue;
        }
        u[ui(i, j, k)] -= (phiAt(i, j, k) - phiAt(i - 1, j, k)) / dx;
      }
    }
  }
  for (int k = 0; k < nz; ++k) {
    for (int j = 0; j <= ny; ++j) {
      for (int i = 0; i < nx; ++i) {
        if (grid_.isSolid(i, j - 1, k) || grid_.isSolid(i, j, k)) {
          continue;
        }
        v[vi(i, j, k)] -= (phiAt(i, j, k) - phiAt(i, j - 1, k)) / dy;
      }
    }
  }
  if (nz > 1) {
    for (int k = 0; k <= nz; ++k) {
      for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
          if (grid_.isSolid(i, j, k - 1) || grid_.isSolid(i, j, k)) {
            continue;
          }
          w[wi(i, j, k)] -= (phiAt(i, j, k) - phiAt(i, j, k - 1)) / dz;
        }
      }
    }
  }
  // Re-enforce boundary velocities but DO NOT touch the outflow: the projection
  // has just set the divergence-free outlet face, and the convective copy would
  // overwrite it and re-inject divergence at the outflow column.
  applyVelocityBC(u, v, w, /*enforceOutflow=*/false);

  static const bool kPoissonDebug = std::getenv("NABLA_POISSON_DEBUG") != nullptr;
  if (kPoissonDebug) {
    std::fprintf(stderr, "[poisson] solver=%s iters=%d rel_res=%.3e conv=%d | divL2=%.3e\n",
                 poisson_->name().c_str(), rep.iterations, rep.residual,
                 rep.converged ? 1 : 0, divergenceL2(u, v, w));
  }

  // Accumulate pressure (p = phi / dt).
  for (std::size_t c = 0; c < p_.size(); ++c) {
    p_[c] = phi[c] / dt;
  }
}

double NSSolver::computeDt() const {
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  const bool twoD = (nz == 1);
  const double visc = 2.0 * nu_ *
                      (1.0 / (grid_.dx * grid_.dx) + 1.0 / (grid_.dy * grid_.dy) +
                       (twoD ? 0.0 : 1.0 / (grid_.dz * grid_.dz)));
  double maxInv = visc + 1e-300;
  for (int k = 0; k < nz; ++k) {
    for (int j = 0; j < ny; ++j) {
      for (int i = 0; i < nx; ++i) {
        if (grid_.isSolid(i, j, k)) {
          continue;
        }
        const double uc = std::abs(0.5 * (u_[ui(i, j, k)] + u_[ui(i + 1, j, k)]));
        const double vc = std::abs(0.5 * (v_[vi(i, j, k)] + v_[vi(i, j + 1, k)]));
        const double wc = twoD ? 0.0 : std::abs(0.5 * (w_[wi(i, j, k)] + w_[wi(i, j, k + 1)]));
        const double inv = uc / grid_.dx + vc / grid_.dy + (twoD ? 0.0 : wc / grid_.dz) + visc;
        maxInv = std::max(maxInv, inv);
      }
    }
  }
  return cfg_.cfl / maxInv;
}

double NSSolver::divergenceL2(const std::vector<double>& u, const std::vector<double>& v,
                              const std::vector<double>& w) const {
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  double s = 0.0;
  std::size_t n = 0;
  for (int k = 0; k < nz; ++k) {
    for (int j = 0; j < ny; ++j) {
      for (int i = 0; i < nx; ++i) {
        if (grid_.isSolid(i, j, k)) {
          continue;
        }
        double div = (u[ui(i + 1, j, k)] - u[ui(i, j, k)]) / grid_.dx +
                     (v[vi(i, j + 1, k)] - v[vi(i, j, k)]) / grid_.dy;
        if (nz > 1) {
          div += (w[wi(i, j, k + 1)] - w[wi(i, j, k)]) / grid_.dz;
        }
        s += div * div;
        ++n;
      }
    }
  }
  return n ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}

double NSSolver::divergenceMax(const std::vector<double>& u, const std::vector<double>& v,
                               const std::vector<double>& w) const {
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  double m = 0.0;
  for (int k = 0; k < nz; ++k) {
    for (int j = 0; j < ny; ++j) {
      for (int i = 0; i < nx; ++i) {
        if (grid_.isSolid(i, j, k)) {
          continue;
        }
        double div = (u[ui(i + 1, j, k)] - u[ui(i, j, k)]) / grid_.dx +
                     (v[vi(i, j + 1, k)] - v[vi(i, j, k)]) / grid_.dy;
        if (nz > 1) {
          div += (w[wi(i, j, k + 1)] - w[wi(i, j, k)]) / grid_.dz;
        }
        m = std::max(m, std::abs(div));
      }
    }
  }
  return m;
}

double NSSolver::massFluxError(const std::vector<double>& u, const std::vector<double>& v,
                               const std::vector<double>& w) const {
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  const double ax = grid_.dy * grid_.dz, ay = grid_.dx * grid_.dz, az = grid_.dx * grid_.dy;
  double net = 0.0;
  for (int k = 0; k < nz; ++k) {
    for (int j = 0; j < ny; ++j) {
      net += (u[ui(0, j, k)] - u[ui(nx, j, k)]) * ax;  // in at x-min, out at x-max
    }
  }
  for (int k = 0; k < nz; ++k) {
    for (int i = 0; i < nx; ++i) {
      net += (v[vi(i, 0, k)] - v[vi(i, ny, k)]) * ay;
    }
  }
  if (nz > 1) {
    for (int j = 0; j < ny; ++j) {
      for (int i = 0; i < nx; ++i) {
        net += (w[wi(i, j, 0)] - w[wi(i, j, nz)]) * az;
      }
    }
  }
  const double scale = cfg_.velocityScale * (grid_.ny * ay) + 1e-300;
  return std::abs(net) / scale;
}

double NSSolver::cellU(int i, int j, int k) const {
  return 0.5 * (u_[ui(i, j, k)] + u_[ui(i + 1, j, k)]);
}
double NSSolver::cellV(int i, int j, int k) const {
  return 0.5 * (v_[vi(i, j, k)] + v_[vi(i, j + 1, k)]);
}
double NSSolver::cellW(int i, int j, int k) const {
  if (grid_.nz == 1) {
    return 0.0;
  }
  return 0.5 * (w_[wi(i, j, k)] + w_[wi(i, j, k + 1)]);
}

std::vector<double> NSSolver::cellVelocity(int component) const {
  std::vector<double> out(grid_.cellCount(), 0.0);
  for (int k = 0; k < grid_.nz; ++k) {
    for (int j = 0; j < grid_.ny; ++j) {
      for (int i = 0; i < grid_.nx; ++i) {
        out[ci(i, j, k)] = component == 0 ? cellU(i, j, k)
                                          : (component == 1 ? cellV(i, j, k) : cellW(i, j, k));
      }
    }
  }
  return out;
}

void NSSolver::computeForces(double& cd, double& cl) const {
  cd = 0.0;
  cl = 0.0;
  if (!hasSolid_) {
    return;
  }
  const int nx = grid_.nx, ny = grid_.ny, nz = grid_.nz;
  const double areaX = grid_.dy * grid_.dz;
  const double areaY = grid_.dx * grid_.dz;
  const double areaZ = grid_.dx * grid_.dy;
  const double mu = nu_ * cfg_.density;
  double fx = 0.0, fy = 0.0;

  // Iterate fluid cells adjacent to a solid; outward body normal points
  // solid->fluid. Pressure: F = -p n_out A. Viscous: mu * u_t/(0.5 d) A.
  const auto accumulate = [&](int fi, int fj, int fk, int si, int sj, int sk,
                              double noutx, double nouty, double area, int normalAxis,
                              double dnorm) {
    if (!grid_.isSolid(si, sj, sk) || grid_.isSolid(fi, fj, fk)) {
      return;
    }
    if (fi < 0 || fi >= nx || fj < 0 || fj >= ny || fk < 0 || fk >= nz) {
      return;
    }
    const double pf = p_[ci(fi, fj, fk)];
    fx += -pf * noutx * area;
    fy += -pf * nouty * area;
    const double uc = cellU(fi, fj, fk);
    const double vc = cellV(fi, fj, fk);
    if (normalAxis != 0) {
      fx += mu * uc / (0.5 * dnorm) * area;  // x is tangential
    }
    if (normalAxis != 1) {
      fy += mu * vc / (0.5 * dnorm) * area;  // y is tangential
    }
  };

  for (int k = 0; k < nz; ++k) {
    for (int j = 0; j < ny; ++j) {
      for (int i = 0; i < nx; ++i) {
        // fluid cell (i,j,k); check its 6 neighbors for solids. The outward body
        // normal points from the solid toward the fluid cell.
        accumulate(i, j, k, i - 1, j, k, 1.0, 0.0, areaX, 0, grid_.dx);   // solid at -x
        accumulate(i, j, k, i + 1, j, k, -1.0, 0.0, areaX, 0, grid_.dx);  // solid at +x
        accumulate(i, j, k, i, j - 1, k, 0.0, 1.0, areaY, 1, grid_.dy);   // solid at -y
        accumulate(i, j, k, i, j + 1, k, 0.0, -1.0, areaY, 1, grid_.dy);  // solid at +y
        if (nz > 1) {
          accumulate(i, j, k, i, j, k - 1, 0.0, 0.0, areaZ, 2, grid_.dz);
          accumulate(i, j, k, i, j, k + 1, 0.0, 0.0, areaZ, 2, grid_.dz);
        }
      }
    }
  }
  double aref = cfg_.refArea;
  if (aref <= 0.0) {
    aref = cfg_.refLength * cfg_.refLength;  // frontal area ~ h^2
  }
  const double q = 0.5 * cfg_.density * cfg_.velocityScale * cfg_.velocityScale * aref + 1e-300;
  cd = fx / q;
  cl = fy / q;
}

StepReport NSSolver::step() { return step(computeDt()); }

StepReport NSSolver::step(double dt) {
  const std::vector<double> u0 = u_, v0 = v_, w0 = w_;
  std::vector<double> ru, rv, rw;
  int it1 = 0, it2 = 0, it3 = 0;
  double pr1 = 0, pr2 = 0, pr3 = 0;

  // SSP-RK3 with a projection after each stage.
  // stage 1: u1 = u^n + dt R(u^n);  project
  applyVelocityBC(u_, v_, w_);
  computeRhs(u_, v_, w_, ru, rv, rw);
  for (std::size_t m = 0; m < u_.size(); ++m) u_[m] = u0[m] + dt * ru[m];
  for (std::size_t m = 0; m < v_.size(); ++m) v_[m] = v0[m] + dt * rv[m];
  for (std::size_t m = 0; m < w_.size(); ++m) w_[m] = w0[m] + dt * rw[m];
  project(u_, v_, w_, dt, it1, pr1);

  // stage 2: u2 = 3/4 u^n + 1/4 (u1 + dt R(u1));  project
  computeRhs(u_, v_, w_, ru, rv, rw);
  for (std::size_t m = 0; m < u_.size(); ++m) u_[m] = 0.75 * u0[m] + 0.25 * (u_[m] + dt * ru[m]);
  for (std::size_t m = 0; m < v_.size(); ++m) v_[m] = 0.75 * v0[m] + 0.25 * (v_[m] + dt * rv[m]);
  for (std::size_t m = 0; m < w_.size(); ++m) w_[m] = 0.75 * w0[m] + 0.25 * (w_[m] + dt * rw[m]);
  project(u_, v_, w_, dt, it2, pr2);

  // stage 3: u^{n+1} = 1/3 u^n + 2/3 (u2 + dt R(u2));  project
  computeRhs(u_, v_, w_, ru, rv, rw);
  const double c1 = 1.0 / 3.0, c2 = 2.0 / 3.0;
  for (std::size_t m = 0; m < u_.size(); ++m) u_[m] = c1 * u0[m] + c2 * (u_[m] + dt * ru[m]);
  for (std::size_t m = 0; m < v_.size(); ++m) v_[m] = c1 * v0[m] + c2 * (v_[m] + dt * rv[m]);
  for (std::size_t m = 0; m < w_.size(); ++m) w_[m] = c1 * w0[m] + c2 * (w_[m] + dt * rw[m]);
  project(u_, v_, w_, dt, it3, pr3);

  // residuals
  double du = 0.0;
  std::size_t n = 0;
  for (std::size_t m = 0; m < u_.size(); ++m) { du += (u_[m] - u0[m]) * (u_[m] - u0[m]); ++n; }
  for (std::size_t m = 0; m < v_.size(); ++m) { du += (v_[m] - v0[m]) * (v_[m] - v0[m]); ++n; }
  for (std::size_t m = 0; m < w_.size(); ++m) { du += (w_[m] - w0[m]) * (w_[m] - w0[m]); ++n; }
  const double momRes =
      std::sqrt(du / static_cast<double>(n ? n : 1)) / (dt * cfg_.velocityScale + 1e-300);

  StepReport rep;
  step_ += 1;
  t_ += dt;
  dtLast_ = dt;
  rep.step = step_;
  rep.t = t_;
  rep.dt = dt;
  rep.cfl = cfg_.cfl;
  rep.momentumResidual = momRes;
  rep.continuityResidual = divergenceL2(u_, v_, w_);
  rep.divergenceMax = divergenceMax(u_, v_, w_);
  rep.massError = massFluxError(u_, v_, w_);
  rep.poissonIters = it1 + it2 + it3;
  rep.poissonResidual = std::max({pr1, pr2, pr3});
  computeForces(rep.cd, rep.cl);
  rep.cellCount = grid_.cellCount();
  rep.accepted = rep.continuityResidual < cfg_.tolContinuity && rep.massError < cfg_.tolMass;
  return rep;
}

void NSSolver::setState(std::vector<double> u, std::vector<double> v, std::vector<double> w,
                        std::vector<double> p, int step, double t, double dtLast) {
  if (u.size() != u_.size() || v.size() != v_.size() || w.size() != w_.size() ||
      p.size() != p_.size()) {
    throw std::runtime_error("NSSolver::setState: size mismatch");
  }
  u_ = std::move(u);
  v_ = std::move(v);
  w_ = std::move(w);
  p_ = std::move(p);
  step_ = step;
  t_ = t;
  dtLast_ = dtLast;
}

}  // namespace nabla::flow
