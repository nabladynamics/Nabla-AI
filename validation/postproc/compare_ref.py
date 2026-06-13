"""Comparison engine: computed metrics vs the reference registry.

Acceptance bands follow the Phase-0 spec: Strouhal, mean Cd/Cl and Cp must
agree within 5-10% (PASS <= inner band, MARGINAL <= outer band, FAIL beyond).
Recirculation lengths use a documented harness default of 10-20% (digitized
lengths carry larger uncertainty). Metrics whose reference slot is still TODO
are reported as NO-REF — they are never silently passed.
"""

from __future__ import annotations

import csv
from dataclasses import dataclass
from pathlib import Path

# metric -> (inner band, outer band), relative
BANDS: dict[str, tuple[float, float]] = {
    "st": (0.05, 0.10),
    "cd_mean": (0.05, 0.10),
    "cl_mean": (0.05, 0.10),
    "cp_front_center": (0.05, 0.10),
}
DEFAULT_BAND = (0.10, 0.20)  # harness default for lengths/volumes (documented)

STATUS_PASS = "PASS"
STATUS_MARGINAL = "MARGINAL"
STATUS_FAIL = "FAIL"
STATUS_TODO = "NO-REF (TODO)"
STATUS_NA = "N/A"


@dataclass
class RefEntry:
    metric: str
    value: float | None
    units: str
    source_id: str
    notes: str


@dataclass
class ComparisonRow:
    metric: str
    computed: float | None
    reference: float | None
    rel_error: float | None
    band: tuple[float, float]
    status: str
    source_id: str
    notes: str


def load_reference_metrics(csv_path: str | Path) -> dict[str, RefEntry]:
    """Load metrics.csv. Non-numeric values (e.g. 'TODO') become None — the
    comparison engine will mark those rows NO-REF rather than fabricate."""
    out: dict[str, RefEntry] = {}
    path = Path(csv_path)
    if not path.exists():
        return out
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            metric = (row.get("metric") or "").strip()
            if not metric or metric.startswith("#"):
                continue
            raw = (row.get("value") or "").strip()
            try:
                value: float | None = float(raw)
            except ValueError:
                value = None
            out[metric] = RefEntry(
                metric=metric,
                value=value,
                units=(row.get("units") or "").strip(),
                source_id=(row.get("source_id") or "").strip(),
                notes=(row.get("notes") or "").strip(),
            )
    return out


def compare(
    computed: dict[str, float | None], reference: dict[str, RefEntry]
) -> list[ComparisonRow]:
    rows: list[ComparisonRow] = []
    metrics = list(dict.fromkeys(list(computed.keys()) + list(reference.keys())))
    for metric in metrics:
        c = computed.get(metric)
        ref = reference.get(metric)
        band = BANDS.get(metric, DEFAULT_BAND)
        r = ref.value if ref else None
        source = ref.source_id if ref else ""
        notes = ref.notes if ref else "metric not in reference registry"

        if c is None:
            status, err = STATUS_NA, None  # not computable from this run (e.g. steady wake -> no St)
        elif r is None:
            status, err = STATUS_TODO, None
        elif r == 0.0:
            err = abs(c - r)
            status = STATUS_PASS if err <= band[0] else (STATUS_MARGINAL if err <= band[1] else STATUS_FAIL)
        else:
            err = abs(c - r) / abs(r)
            status = STATUS_PASS if err <= band[0] else (STATUS_MARGINAL if err <= band[1] else STATUS_FAIL)
        rows.append(ComparisonRow(metric, c, r, err, band, status, source, notes))
    return rows


def summary_counts(rows: list[ComparisonRow]) -> dict[str, int]:
    out = {STATUS_PASS: 0, STATUS_MARGINAL: 0, STATUS_FAIL: 0, STATUS_TODO: 0, STATUS_NA: 0}
    for r in rows:
        out[r.status] = out.get(r.status, 0) + 1
    return out
