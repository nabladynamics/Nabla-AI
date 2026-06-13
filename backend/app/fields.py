"""Server-side field bundles for the Post-simulation viewers.

Parses a uniform-grid snapshot .vtu, optionally decimates it (stride), derives
visualization fields (speed, kinetic energy, vorticity magnitude, Q-criterion)
and returns everything as base64-encoded Float32 arrays — compact to ship,
trivial to ingest as typed arrays in the browser. Results are cached by file
mtime + stride.
"""

from __future__ import annotations

import base64
import math
import xml.etree.ElementTree as ET
from array import array
from pathlib import Path

from pydantic import BaseModel


class FieldBundle(BaseModel):
    step: int
    source: str
    dims: tuple[int, int, int]  # decimated cell dims
    origin: tuple[float, float, float]
    spacing: tuple[float, float, float]
    stride: int
    fields: dict[str, str]  # name -> base64(Float32 little-endian)
    ranges: dict[str, tuple[float, float]]


class UniformField:
    def __init__(
        self,
        nx: int,
        ny: int,
        nz: int,
        dx: float,
        dy: float,
        dz: float,
        x0: float,
        y0: float,
        z0: float,
    ) -> None:
        self.nx, self.ny, self.nz = nx, ny, nz
        self.dx, self.dy, self.dz = dx, dy, dz
        self.x0, self.y0, self.z0 = x0, y0, z0
        self.arrays: dict[str, list[float]] = {}

    def idx(self, i: int, j: int, k: int) -> int:
        return i + self.nx * (j + self.ny * k)


def parse_uniform_vtu(path: Path) -> UniformField:
    root = ET.parse(path).getroot()
    piece = root.find(".//Piece")
    if piece is None:
        raise ValueError(f"{path.name}: no <Piece>")
    ncells = int(piece.get("NumberOfCells", "0"))
    points_node = piece.find("./Points/DataArray")
    if points_node is None or not points_node.text:
        raise ValueError(f"{path.name}: no points")
    pts = [float(v) for v in points_node.text.split()]

    centers_x: list[float] = []
    centers_y: list[float] = []
    centers_z: list[float] = []
    for c in range(ncells):
        base = c * 24
        centers_x.append(sum(pts[base : base + 24 : 3]) / 8)
        centers_y.append(sum(pts[base + 1 : base + 24 : 3]) / 8)
        centers_z.append(sum(pts[base + 2 : base + 24 : 3]) / 8)

    xs = sorted({round(v, 7) for v in centers_x})
    ys = sorted({round(v, 7) for v in centers_y})
    zs = sorted({round(v, 7) for v in centers_z})
    nx, ny, nz = len(xs), len(ys), len(zs)
    if nx * ny * nz != ncells:
        raise ValueError(f"{path.name}: not a uniform grid ({ncells} cells vs {nx}x{ny}x{nz})")
    dx = xs[1] - xs[0] if nx > 1 else 1.0
    dy = ys[1] - ys[0] if ny > 1 else 1.0
    dz = zs[1] - zs[0] if nz > 1 else 1.0
    grid = UniformField(nx, ny, nz, dx, dy, dz, xs[0] - dx / 2, ys[0] - dy / 2, zs[0] - dz / 2)

    xi = {v: i for i, v in enumerate(xs)}
    yi = {v: j for j, v in enumerate(ys)}
    zi = {v: k for k, v in enumerate(zs)}
    perm = [0] * ncells
    for file_index in range(ncells):
        c = grid.idx(
            xi[round(centers_x[file_index], 7)],
            yi[round(centers_y[file_index], 7)],
            zi[round(centers_z[file_index], 7)],
        )
        perm[c] = file_index

    cell_data = piece.find("./CellData")
    if cell_data is not None:
        for node in cell_data.iter("DataArray"):
            name = node.get("Name", "")
            if name and node.text:
                raw = [float(v) for v in node.text.split()]
                grid.arrays[name] = [raw[perm[c]] for c in range(ncells)]
    return grid


def decimate(grid: UniformField, stride: int) -> UniformField:
    if stride <= 1:
        return grid
    nx = (grid.nx + stride - 1) // stride
    ny = (grid.ny + stride - 1) // stride
    nz = (grid.nz + stride - 1) // stride
    out = UniformField(
        nx, ny, nz, grid.dx * stride, grid.dy * stride, grid.dz * stride,
        grid.x0, grid.y0, grid.z0,
    )
    for name, values in grid.arrays.items():
        slim = [0.0] * (nx * ny * nz)
        for k in range(nz):
            for j in range(ny):
                for i in range(nx):
                    slim[out.idx(i, j, k)] = values[
                        grid.idx(
                            min(i * stride, grid.nx - 1),
                            min(j * stride, grid.ny - 1),
                            min(k * stride, grid.nz - 1),
                        )
                    ]
        out.arrays[name] = slim
    return out


def _grad(grid: UniformField, f: list[float], i: int, j: int, k: int) -> tuple[float, float, float]:
    def d(axis: int) -> float:
        n = (grid.nx, grid.ny, grid.nz)[axis]
        h = (grid.dx, grid.dy, grid.dz)[axis]
        lo = [i, j, k]
        hi = [i, j, k]
        lo[axis] = max(0, lo[axis] - 1)
        hi[axis] = min(n - 1, hi[axis] + 1)
        span = (hi[axis] - lo[axis]) * h
        if span == 0.0:
            return 0.0
        return (f[grid.idx(hi[0], hi[1], hi[2])] - f[grid.idx(lo[0], lo[1], lo[2])]) / span

    return d(0), d(1), d(2)


def add_derived_fields(grid: UniformField) -> None:
    """speed, ke (kinetic energy 0.5|u|^2), vort (|omega|), q (Q-criterion)."""
    u = grid.arrays.get("u")
    v = grid.arrays.get("v")
    w = grid.arrays.get("w")
    if u is None or v is None or w is None:
        return
    n = grid.nx * grid.ny * grid.nz
    solid = grid.arrays.get("solid", [0.0] * n)
    speed = [0.0] * n
    ke = [0.0] * n
    vort = [0.0] * n
    q = [0.0] * n
    for k in range(grid.nz):
        for j in range(grid.ny):
            for i in range(grid.nx):
                c = grid.idx(i, j, k)
                s = math.sqrt(u[c] ** 2 + v[c] ** 2 + w[c] ** 2)
                speed[c] = s
                ke[c] = 0.5 * s * s
                if solid[c] > 0.5:
                    continue
                du = _grad(grid, u, i, j, k)
                dv = _grad(grid, v, i, j, k)
                dw = _grad(grid, w, i, j, k)
                ox = dw[1] - dv[2]
                oy = du[2] - dw[0]
                oz = dv[0] - du[1]
                vort[c] = math.sqrt(ox * ox + oy * oy + oz * oz)
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
    grid.arrays["speed"] = speed
    grid.arrays["ke"] = ke
    grid.arrays["vort"] = vort
    grid.arrays["q"] = q


_BUNDLE_FIELDS = ("u", "v", "w", "p", "speed", "ke", "vort", "q", "solid")
_CACHE: dict[tuple[str, float, int], FieldBundle] = {}


def field_bundle(path: Path, step: int, stride: int = 0, max_cells: int = 300_000) -> FieldBundle:
    mtime = path.stat().st_mtime
    raw = parse_uniform_vtu(path)
    if stride <= 0:  # auto-decimate to the cell budget (laptop-GPU friendly)
        total = raw.nx * raw.ny * raw.nz
        stride = max(1, math.ceil((total / max_cells) ** (1 / 3)))
    key = (str(path), mtime, stride)
    cached = _CACHE.get(key)
    if cached is not None:
        return cached

    grid = decimate(raw, stride)
    add_derived_fields(grid)

    fields: dict[str, str] = {}
    ranges: dict[str, tuple[float, float]] = {}
    for name in _BUNDLE_FIELDS:
        values = grid.arrays.get(name)
        if values is None:
            continue
        packed = array("f", values)
        fields[name] = base64.b64encode(packed.tobytes()).decode("ascii")
        ranges[name] = (min(values), max(values))

    bundle = FieldBundle(
        step=step,
        source=path.name,
        dims=(grid.nx, grid.ny, grid.nz),
        origin=(grid.x0 + grid.dx / 2, grid.y0 + grid.dy / 2, grid.z0 + grid.dz / 2),
        spacing=(grid.dx, grid.dy, grid.dz),
        stride=stride,
        fields=fields,
        ranges=ranges,
    )
    if len(_CACHE) > 8:
        _CACHE.clear()
    _CACHE[key] = bundle
    return bundle
