"""Server-side fidelity-map slices.

The Simulation dashboard must never load full 3D fields: this module parses the
solver's latest octree snapshot (``adaptive_latest.vtu``, falling back to the
final/ingest meshes) and extracts the cells intersecting a centerplane, with
their refinement level and physics-model label. Results are cached by file
mtime so the live poll is cheap.
"""

from __future__ import annotations

import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Literal

from pydantic import BaseModel

MODE_NAMES = ("FULL_NS", "NEAR_WALL", "LAMINAR_BL", "INVISCID", "WAKE_SHEAR")
SOLID_MODE = 99  # mask == InsideSolid; rendered dark by the frontend

SNAPSHOT_CANDIDATES = ("adaptive_latest.vtu", "adaptive_final.vtu", "mesh.vtu")

SliceAxis = Literal["y", "z"]


class SliceCell(BaseModel):
    x: float  # rect lower corner in slice coordinates
    y: float
    w: float
    h: float
    level: int
    mode: int | None  # index into MODE_NAMES, SOLID_MODE, or None (no labels)


class SliceResponse(BaseModel):
    axis: SliceAxis
    coord: float
    bounds: tuple[float, float, float, float]  # (x0, x1, y0, y1) in slice coords
    source: str
    updated: float  # source file mtime (epoch seconds)
    cells: list[SliceCell]
    mode_counts: dict[str, int]


class _Parsed:
    def __init__(self) -> None:
        self.centers: list[tuple[float, float, float]] = []
        self.sizes: list[tuple[float, float, float]] = []
        self.levels: list[int] = []
        self.modes: list[int | None] = []


def _parse_octree_vtu(path: Path) -> _Parsed:
    root = ET.parse(path).getroot()
    piece = root.find(".//Piece")
    if piece is None:
        raise ValueError(f"{path}: no <Piece>")
    ncells = int(piece.get("NumberOfCells", "0"))
    points_node = piece.find("./Points/DataArray")
    if points_node is None or not points_node.text:
        raise ValueError(f"{path}: no points")
    pts = [float(v) for v in points_node.text.split()]

    arrays: dict[str, list[float]] = {}
    cell_data = piece.find("./CellData")
    if cell_data is not None:
        for array in cell_data.iter("DataArray"):
            name = array.get("Name", "")
            if name in ("level", "mask", "physics_mode") and array.text:
                arrays[name] = [float(v) for v in array.text.split()]

    parsed = _Parsed()
    levels = arrays.get("level")
    masks = arrays.get("mask")
    modes = arrays.get("physics_mode")
    for c in range(ncells):
        base = c * 24  # 8 corner points x 3 components
        xs = pts[base : base + 24 : 3]
        ys = pts[base + 1 : base + 24 : 3]
        zs = pts[base + 2 : base + 24 : 3]
        parsed.centers.append(
            ((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2)
        )
        parsed.sizes.append((max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)))
        parsed.levels.append(int(levels[c]) if levels else 0)
        if masks is not None and masks[c] > 1.5:  # InsideSolid
            parsed.modes.append(SOLID_MODE)
        elif modes is not None:
            parsed.modes.append(int(modes[c]))
        else:
            parsed.modes.append(None)
    return parsed


def _extract(
    parsed: _Parsed, axis: SliceAxis, frac: float, source: str, mtime: float
) -> SliceResponse:
    # slice-plane coordinate from the snapshot's own bounds
    axis_index = 1 if axis == "y" else 2
    lo = min(
        c[axis_index] - s[axis_index] / 2
        for c, s in zip(parsed.centers, parsed.sizes, strict=True)
    )
    hi = max(
        c[axis_index] + s[axis_index] / 2
        for c, s in zip(parsed.centers, parsed.sizes, strict=True)
    )
    coord = lo + frac * (hi - lo)

    # projection: slicing z -> (x, y) plane; slicing y -> (x, z) plane
    u_index, v_index = (0, 2) if axis == "y" else (0, 1)

    cells: list[SliceCell] = []
    counts: dict[str, int] = {}
    for center, size, level, mode in zip(
        parsed.centers, parsed.sizes, parsed.levels, parsed.modes, strict=True
    ):
        half = size[axis_index] / 2
        if abs(center[axis_index] - coord) > half + 1e-12:
            continue
        cells.append(
            SliceCell(
                x=center[u_index] - size[u_index] / 2,
                y=center[v_index] - size[v_index] / 2,
                w=size[u_index],
                h=size[v_index],
                level=level,
                mode=mode,
            )
        )
        if mode is None:
            continue
        if mode == SOLID_MODE:
            name = "SOLID"
        else:
            name = MODE_NAMES[mode] if mode < len(MODE_NAMES) else str(mode)
        counts[name] = counts.get(name, 0) + 1

    if cells:
        x0 = min(c.x for c in cells)
        x1 = max(c.x + c.w for c in cells)
        y0 = min(c.y for c in cells)
        y1 = max(c.y + c.h for c in cells)
    else:
        x0 = x1 = y0 = y1 = 0.0
    return SliceResponse(
        axis=axis,
        coord=coord,
        bounds=(x0, x1, y0, y1),
        source=source,
        updated=mtime,
        cells=cells,
        mode_counts=counts,
    )


_CACHE: dict[tuple[str, float, str, float], SliceResponse] = {}


def fidelity_slice(workspace: Path, axis: SliceAxis, frac: float) -> SliceResponse | None:
    """Slice the freshest snapshot in the workspace; None when none exists."""
    source: Path | None = None
    for name in SNAPSHOT_CANDIDATES:
        candidate = workspace / name
        if candidate.is_file():
            source = candidate
            break
    if source is None:
        return None

    frac = min(max(frac, 0.0), 1.0)
    mtime = source.stat().st_mtime
    key = (str(source), mtime, axis, round(frac, 4))
    cached = _CACHE.get(key)
    if cached is not None:
        return cached

    response = _extract(_parse_octree_vtu(source), axis, frac, source.name, mtime)
    if len(_CACHE) > 16:
        _CACHE.clear()
    _CACHE[key] = response
    return response
