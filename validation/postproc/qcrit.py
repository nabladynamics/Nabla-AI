"""Q-criterion on the uniform mean field + isosurface export.

Q = 1/2 (||Omega||^2 - ||S||^2) from central differences (one-sided at domain
boundaries and solid cells). The isosurface export is dependency-free: every
cell face separating Q >= threshold from Q < threshold is emitted as a quad
into a Wavefront .obj (a voxel isosurface any 3D viewer opens).
"""

from __future__ import annotations

from pathlib import Path

from validation.postproc.vtu import UniformGrid


def _ddx(grid: UniformGrid, f: list[float], i: int, j: int, k: int, axis: int) -> float:
    n = (grid.nx, grid.ny, grid.nz)[axis]
    d = (grid.dx, grid.dy, grid.dz)[axis]
    idx = [i, j, k]
    lo, hi = list(idx), list(idx)
    lo[axis] = max(0, idx[axis] - 1)
    hi[axis] = min(n - 1, idx[axis] + 1)
    span = (hi[axis] - lo[axis]) * d
    if span == 0.0:
        return 0.0
    return (f[grid.cidx(hi[0], hi[1], hi[2])] - f[grid.cidx(lo[0], lo[1], lo[2])]) / span


def compute_q(
    grid: UniformGrid, u: list[float], v: list[float], w: list[float], solid: list[float]
) -> list[float]:
    q = [0.0] * grid.cells
    for k in range(grid.nz):
        for j in range(grid.ny):
            for i in range(grid.nx):
                c = grid.cidx(i, j, k)
                if solid[c] > 0.5:
                    continue
                du = [_ddx(grid, u, i, j, k, a) for a in range(3)]
                dv = [_ddx(grid, v, i, j, k, a) for a in range(3)]
                dw = [_ddx(grid, w, i, j, k, a) for a in range(3)]
                ss = du[0] ** 2 + dv[1] ** 2 + dw[2] ** 2 + 2.0 * (
                    (0.5 * (du[1] + dv[0])) ** 2
                    + (0.5 * (du[2] + dw[0])) ** 2
                    + (0.5 * (dv[2] + dw[1])) ** 2
                )
                oo = 2.0 * (
                    (0.5 * (du[1] - dv[0])) ** 2
                    + (0.5 * (du[2] - dw[0])) ** 2
                    + (0.5 * (dv[2] - dw[1])) ** 2
                )
                q[c] = 0.5 * (oo - ss)
    return q


def export_q_isosurface_obj(
    grid: UniformGrid, q: list[float], threshold: float, path: str | Path
) -> int:
    """Write the blocky Q >= threshold isosurface as .obj quads; returns quad count."""

    def above(i: int, j: int, k: int) -> bool:
        if i < 0 or i >= grid.nx or j < 0 or j >= grid.ny or k < 0 or k >= grid.nz:
            return False
        return q[grid.cidx(i, j, k)] >= threshold

    verts: dict[tuple[float, float, float], int] = {}
    faces: list[tuple[int, int, int, int]] = []

    def vid(x: float, y: float, z: float) -> int:
        key = (round(x, 9), round(y, 9), round(z, 9))
        if key not in verts:
            verts[key] = len(verts) + 1  # OBJ indices are 1-based
        return verts[key]

    for k in range(grid.nz):
        for j in range(grid.ny):
            for i in range(grid.nx):
                if not above(i, j, k):
                    continue
                cx, cy, cz = grid.center(i, j, k)
                hx, hy, hz = grid.dx / 2, grid.dy / 2, grid.dz / 2
                x0, x1 = cx - hx, cx + hx
                y0, y1 = cy - hy, cy + hy
                z0, z1 = cz - hz, cz + hz
                # face emitted when the neighbor across it is below threshold
                if not above(i - 1, j, k):
                    faces.append((vid(x0, y0, z0), vid(x0, y1, z0), vid(x0, y1, z1), vid(x0, y0, z1)))
                if not above(i + 1, j, k):
                    faces.append((vid(x1, y0, z0), vid(x1, y0, z1), vid(x1, y1, z1), vid(x1, y1, z0)))
                if not above(i, j - 1, k):
                    faces.append((vid(x0, y0, z0), vid(x0, y0, z1), vid(x1, y0, z1), vid(x1, y0, z0)))
                if not above(i, j + 1, k):
                    faces.append((vid(x0, y1, z0), vid(x1, y1, z0), vid(x1, y1, z1), vid(x0, y1, z1)))
                if not above(i, j, k - 1):
                    faces.append((vid(x0, y0, z0), vid(x1, y0, z0), vid(x1, y1, z0), vid(x0, y1, z0)))
                if not above(i, j, k + 1):
                    faces.append((vid(x0, y0, z1), vid(x0, y1, z1), vid(x1, y1, z1), vid(x1, y0, z1)))

    with open(path, "w") as f:
        f.write(f"# Nabla AI Q-criterion isosurface (Q >= {threshold})\n")
        ordered = sorted(verts.items(), key=lambda kv: kv[1])
        for (x, y, z), _ in ordered:
            f.write(f"v {x} {y} {z}\n")
        for a, b, c, d in faces:
            f.write(f"f {a} {b} {c} {d}\n")
    return len(faces)
