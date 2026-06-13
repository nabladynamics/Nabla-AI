"""Parser for the adaptive layer's audit.jsonl (the decision trail)."""

from __future__ import annotations

import json
import re
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

MODE_NAMES = ("FULL_NS", "NEAR_WALL", "LAMINAR_BL", "INVISCID", "WAKE_SHEAR")
MODE_COLORS = {
    "FULL_NS": "#c0392b",
    "NEAR_WALL": "#e67e22",
    "LAMINAR_BL": "#f1c40f",
    "INVISCID": "#2980b9",
    "WAKE_SHEAR": "#8e44ad",
}

_ACC_RE = re.compile(r"(\d+) accepted, (\d+) rejected")
_GUARD_RE = re.compile(r"(\d+) reduced proposals overridden")


@dataclass
class AcceptanceRow:
    mode: str
    proposals: int = 0
    accepted: int = 0
    rejected: int = 0
    reasons: Counter = field(default_factory=Counter)


@dataclass
class AuditData:
    steps: list[int] = field(default_factory=list)
    cells: list[int] = field(default_factory=list)          # from metrics events
    cd: list[float] = field(default_factory=list)
    cl: list[float] = field(default_factory=list)
    refine_per_step: dict[int, int] = field(default_factory=dict)
    coarsen_per_step: dict[int, int] = field(default_factory=dict)
    refine_reasons: Counter = field(default_factory=Counter)
    acceptance: dict[str, AcceptanceRow] = field(default_factory=dict)
    guard_overrides_total: int = 0
    model_changes: Counter = field(default_factory=Counter)  # (from, to) -> n
    change_reasons: Counter = field(default_factory=Counter)
    step_decisions: Counter = field(default_factory=Counter)
    total_events: int = 0


def load_audit(path: str | Path) -> AuditData:
    data = AuditData()
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        ev = json.loads(line)
        data.total_events += 1
        kind = ev.get("event")
        step = int(ev.get("step", 0))
        if kind == "metrics":
            data.steps.append(step)
            data.cells.append(int(ev.get("cells", 0)))
            data.cd.append(float(ev.get("cd", 0.0)))
            data.cl.append(float(ev.get("cl", 0.0)))
        elif kind == "refine":
            data.refine_per_step[step] = data.refine_per_step.get(step, 0) + int(ev["count"])
            data.refine_reasons[ev.get("reason", "")] += int(ev["count"])
        elif kind == "coarsen":
            data.coarsen_per_step[step] = data.coarsen_per_step.get(step, 0) + int(ev["count"])
        elif kind == "acceptance":
            region = ev.get("region", "")
            mode = ev.get("mode", "")
            reason = ev.get("reason", "")
            if region == "hard-guard-zones":
                m = _GUARD_RE.search(reason)
                if m:
                    data.guard_overrides_total += int(m.group(1))
                continue
            row = data.acceptance.setdefault(mode, AcceptanceRow(mode))
            m = _ACC_RE.search(reason)
            if m:
                acc, rej = int(m.group(1)), int(m.group(2))
                row.proposals += acc + rej
                row.accepted += acc
                row.rejected += rej
            row.reasons[reason] += 1
        elif kind == "model_change":
            data.model_changes[(ev.get("from", "?"), ev.get("to", "?"))] += 1
            data.change_reasons[ev.get("reason", "")] += 1
        elif kind == "step_decision":
            data.step_decisions[ev.get("decision", "?")] += 1
    return data


def parse_efficiency(path: str | Path) -> dict[str, float]:
    """Parse efficiency.txt: 'uniform-fine cells = N, adaptive mean cells = M,
    cell speedup = Sx, wall = W s'."""
    text = Path(path).read_text()
    out: dict[str, float] = {}
    for key, pat in (
        ("uniform_fine_cells", r"uniform-fine cells = ([\d.]+)"),
        ("adaptive_mean_cells", r"adaptive mean cells = ([\d.]+)"),
        ("cell_speedup", r"cell speedup = ([\d.]+)x"),
        ("wall_seconds", r"wall = ([\d.]+) s"),
    ):
        m = re.search(pat, text)
        if m:
            out[key] = float(m.group(1))
    return out
