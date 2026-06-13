"""Readers for the solver's ASCII .vtu output (uniform grid and octree).

Both writers emit one hexahedron per cell with 8 points each; cell-centred
fields live in <CellData>. The uniform reader reorders cells into canonical
``c = i + nx*(j + ny*k)`` order regardless of file order, so downstream code
can index by (i, j, k).
"""

from __future__ import annotations

import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path


@dataclass(frozen=True)
class UniformGrid:
    nx: int
    ny: int
    nz: int
    dx: float
    dy: float
    dz: float
    x0: float
    y0: float
    z0: float

    def cidx(self, i: int, j: int, k: int) -> int:
        return i + self.nx * (j + self.ny * k)

    def center(self, i: int, j: int, k: int) -> tuple[float, float, float]:
        return (
            self.x0 + (i + 0.5) * self.dx,
            self.y0 + (j + 0.5) * self.dy,
            self.z0 + (k + 0.5) * self.dz,
        )

    @property
    def cells(self) -> int:
        return self.nx * self.ny * self.nz

    def cell_volume(self) -> float:
        return self.dx * self.dy * self.dz


@dataclass
class OctreeCells:
    """Octree leaves: per-cell center, size and fields (irregular mesh)."""

    centers: list[tuple[float, float, float]] = field(default_factory=list)
    sizes: list[tuple[float, float, float]] = field(default_factory=list)
    fields: dict[str, list[float]] = field(default_factory=dict)

    @property
    def count(self) -> int:
        return len(self.centers)

    def volume(self, c: int) -> float:
        sx, sy, sz = self.sizes[c]
        return sx * sy * sz


def _parse_points_and_celldata(
    path: Path,
) -> tuple[list[float], dict[str, list[float]], int]:
    root = ET.parse(path).getroot()
    piece = root.find(".//Piece")
    if piece is None:
        raise ValueError(f"{path}: no <Piece> element")
    ncells = int(piece.get("NumberOfCells", "0"))
    pts_node = piece.find("./Points/DataArray")
    if pts_node is None or not pts_node.text:
        raise ValueError(f"{path}: no points")
    points = [float(v) for v in pts_node.text.split()]
    cell_data = piece.find("./CellData")
    fields: dict[str, list[float]] = {}
    if cell_data is not None:
        for da in cell_data.iter("DataArray"):
            name = da.get("Name", "")
            if name and da.text:
                fields[name] = [float(v) for v in da.text.split()]
    return points, fields, ncells


def _cell_centers(points: list[float], ncells: int) -> list[tuple[float, float, float]]:
    centers = []
    for c in range(ncells):
        xs = ys = zs = 0.0
        base = c * 24  # 8 points * 3 components
        for p in range(8):
            xs += points[base + 3 * p]
            ys += points[base + 3 * p + 1]
            zs += points[base + 3 * p + 2]
        centers.append((xs / 8.0, ys / 8.0, zs / 8.0))
    return centers


def _axis_values(values: list[float]) -> list[float]:
    return sorted({round(v, 7) for v in values})


def load_uniform_vtu(path: str | Path) -> tuple[UniformGrid, dict[str, list[float]]]:
    """Load a uniform-grid .vtu; fields returned in canonical (i,j,k) order."""
    path = Path(path)
    points, raw_fields, ncells = _parse_points_and_celldata(path)
    centers = _cell_centers(points, ncells)

    xs = _axis_values([c[0] for c in centers])
    ys = _axis_values([c[1] for c in centers])
    zs = _axis_values([c[2] for c in centers])
    nx, ny, nz = len(xs), len(ys), len(zs)
    if nx * ny * nz != ncells:
        raise ValueError(f"{path}: cells ({ncells}) != nx*ny*nz ({nx}*{ny}*{nz})")
    dx = xs[1] - xs[0] if nx > 1 else 1.0
    dy = ys[1] - ys[0] if ny > 1 else 1.0
    dz = zs[1] - zs[0] if nz > 1 else 1.0
    grid = UniformGrid(nx, ny, nz, dx, dy, dz, xs[0] - dx / 2, ys[0] - dy / 2, zs[0] - dz / 2)

    xi = {v: i for i, v in enumerate(xs)}
    yi = {v: j for j, v in enumerate(ys)}
    zi = {v: k for k, v in enumerate(zs)}
    perm = [0] * ncells  # canonical index -> file index
    for file_idx, (cx, cy, cz) in enumerate(centers):
        c = grid.cidx(xi[round(cx, 7)], yi[round(cy, 7)], zi[round(cz, 7)])
        perm[c] = file_idx

    fields = {name: [vals[perm[c]] for c in range(ncells)] for name, vals in raw_fields.items()}
    return grid, fields


def load_octree_vtu(path: str | Path) -> OctreeCells:
    """Load an octree .vtu (cells of varying size); order preserved."""
    path = Path(path)
    points, fields, ncells = _parse_points_and_celldata(path)
    out = OctreeCells(fields=fields)
    for c in range(ncells):
        base = c * 24
        xs = [points[base + 3 * p] for p in range(8)]
        ys = [points[base + 3 * p + 1] for p in range(8)]
        zs = [points[base + 3 * p + 2] for p in range(8)]
        out.centers.append(((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2))
        out.sizes.append((max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)))
    return out


def solid_bbox(
    grid: UniformGrid, solid: list[float]
) -> tuple[float, float, float, float, float, float] | None:
    """Bounding box (x0,x1,y0,y1,z0,z1) of solid cells, or None if no solid."""
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    found = False
    for k in range(grid.nz):
        for j in range(grid.ny):
            for i in range(grid.nx):
                if solid[grid.cidx(i, j, k)] > 0.5:
                    found = True
                    cx, cy, cz = grid.center(i, j, k)
                    h = (grid.dx / 2, grid.dy / 2, grid.dz / 2)
                    for a, (cc, hh) in enumerate(((cx, h[0]), (cy, h[1]), (cz, h[2]))):
                        lo[a] = min(lo[a], cc - hh)
                        hi[a] = max(hi[a], cc + hh)
    if not found:
        return None
    return (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2])
