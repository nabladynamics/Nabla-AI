"""Mean-field computation over a statistics window of .vtu snapshots."""

from __future__ import annotations

import re
from pathlib import Path

from validation.postproc.vtu import UniformGrid, load_uniform_vtu

_SNAP_RE = re.compile(r"snapshot_(\d+)\.vtu$")


def list_snapshots(run_dir: str | Path) -> list[tuple[int, Path]]:
    """(step, path) for every snapshot in a run dir, sorted by step."""
    out = []
    for p in Path(run_dir).glob("snapshot_*.vtu"):
        m = _SNAP_RE.search(p.name)
        if m:
            out.append((int(m.group(1)), p))
    return sorted(out)


def select_window_snapshots(
    run_dir: str | Path, window_frac: float = 0.5
) -> list[tuple[int, Path]]:
    """Snapshots inside the trailing statistics window (>= (1-frac)*last_step)."""
    snaps = list_snapshots(run_dir)
    if not snaps:
        return []
    last = snaps[-1][0]
    start = last * (1.0 - window_frac)
    return [(s, p) for s, p in snaps if s >= start]


def mean_fields(
    paths: list[Path], names: tuple[str, ...] = ("u", "v", "w", "p")
) -> tuple[UniformGrid, dict[str, list[float]]]:
    """Average the named cell fields over the given snapshots."""
    if not paths:
        raise ValueError("mean_fields: no snapshots provided")
    grid, first = load_uniform_vtu(paths[0])
    acc = {n: list(first[n]) for n in names if n in first}
    if "solid" in first:
        acc["solid"] = list(first["solid"])  # geometry: take from first snapshot
    for p in paths[1:]:
        g, f = load_uniform_vtu(p)
        if g.cells != grid.cells:
            raise ValueError(f"{p}: grid changed between snapshots")
        for n in names:
            if n in f:
                a = acc[n]
                src = f[n]
                for c in range(g.cells):
                    a[c] += src[c]
    inv = 1.0 / len(paths)
    for n in names:
        if n in acc:
            acc[n] = [v * inv for v in acc[n]]
    return grid, acc
