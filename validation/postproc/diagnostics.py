"""Reader + statistics for the solver's per-step diagnostics.jsonl."""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any


def load_diagnostics(path: str | Path) -> list[dict[str, Any]]:
    """Load diagnostics.jsonl. Restarted runs can append duplicate step
    numbers; the LAST occurrence of each step wins. Sorted by step.
    Lines without a "step" key (the build-provenance meta line) are skipped."""
    by_step: dict[int, dict[str, Any]] = {}
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        if "step" not in rec:
            continue
        by_step[int(rec["step"])] = rec
    return [by_step[s] for s in sorted(by_step)]


def load_meta(path: str | Path) -> dict[str, Any] | None:
    """The solver's build-provenance meta line ({"event":"meta", ...} with
    solver_version + git_sha), or None for pre-provenance diagnostics files."""
    p = Path(path)
    if not p.is_file():
        return None
    for line in p.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        rec: dict[str, Any] = json.loads(line)
        if rec.get("event") == "meta":
            return rec
        if "step" in rec:  # step records start immediately: no meta line
            return None
    return None


def stats_window(records: list[dict[str, Any]], window_frac: float = 0.5) -> list[dict[str, Any]]:
    """The trailing fraction of the run used for statistics (skips transient)."""
    if not records:
        return []
    last = records[-1]["step"]
    start = last * (1.0 - window_frac)
    return [r for r in records if r["step"] >= start]


def _mean_std(values: list[float]) -> tuple[float, float]:
    n = len(values)
    if n == 0:
        return 0.0, 0.0
    m = sum(values) / n
    var = sum((v - m) ** 2 for v in values) / n
    return m, math.sqrt(var)


def force_stats(records: list[dict[str, Any]]) -> dict[str, float]:
    """Mean/std/min/max of Cd and Cl plus dt statistics over the records."""
    cd = [float(r["cd"]) for r in records]
    cl = [float(r["cl"]) for r in records]
    dt = [float(r["dt"]) for r in records]
    cd_m, cd_s = _mean_std(cd)
    cl_m, cl_s = _mean_std(cl)
    dt_m, dt_s = _mean_std(dt)
    return {
        "cd_mean": cd_m,
        "cd_std": cd_s,
        "cd_min": min(cd) if cd else 0.0,
        "cd_max": max(cd) if cd else 0.0,
        "cl_mean": cl_m,
        "cl_std": cl_s,
        "cl_min": min(cl) if cl else 0.0,
        "cl_max": max(cl) if cl else 0.0,
        "dt_mean": dt_m,
        "dt_std": dt_s,
        "samples": float(len(records)),
    }
