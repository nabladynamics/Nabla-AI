"""Post-simulation analysis for the dashboard: force statistics, lift PSD with
Strouhal detection, floor critical points + vortex-core estimates, comparison
against the validation reference registry, audit-trail summary and warnings.

The numerical routines mirror /validation's post-processing library (kept in
sync by convention; extracting a shared package is the known refactor).
"""

from __future__ import annotations

import itertools
import json
import math
import re
from pathlib import Path
from typing import Any

from pydantic import BaseModel

from app.fields import UniformField, add_derived_fields, parse_uniform_vtu

# --- models ---------------------------------------------------------------------


class ForceStats(BaseModel):
    samples: int
    t0: float
    t1: float
    cd_mean: float
    cd_std: float
    cl_mean: float
    cl_std: float
    cd_min: float
    cd_max: float
    cl_min: float
    cl_max: float


class Spectrum(BaseModel):
    st: list[float]
    mag: list[float]
    strouhal: float | None
    frequency: float | None
    prominent: bool          # True only for a statistically defensible peak
    resolution_st: float
    snr: float               # peak magnitude / broadband background mean
    cycles: float            # shedding periods of the peak that fit the window
    min_cycles: float        # cycles required before a peak is trustworthy
    window_too_short: bool   # candidate peak exists but the window is too short
    reason: str              # human-readable detection / non-detection reason


class CriticalPoint(BaseModel):
    x: float
    kind: str  # separation | attachment


class CoreMarker(BaseModel):
    label: str
    x: float
    y: float
    z: float
    q: float


class Recirculation(BaseModel):
    x_separation: float | None
    x_reattachment: float | None
    upstream_separation_over_h: float | None
    reattachment_over_h: float | None
    reverse_flow_volume_over_h3: float | None
    critical_points: list[CriticalPoint]
    cores: list[CoreMarker]


class ComparisonRow(BaseModel):
    metric: str
    computed: float | None
    reference: float | None
    rel_error: float | None
    band: tuple[float, float]
    status: str
    source: str


class AuditSummary(BaseModel):
    accepts: int
    rejects: int
    promotions: int
    guard_overrides: int
    refinements: int
    coarsenings: int
    cell_speedup: float | None
    adaptive_mean_cells: float | None
    uniform_fine_cells: float | None


class AnalysisResponse(BaseModel):
    window_frac: float
    forces: ForceStats | None
    spectrum: Spectrum | None
    recirculation: Recirculation | None
    comparison: list[ComparisonRow]
    audit: AuditSummary | None
    warnings: list[str]
    reference_re: int | None


# --- diagnostics / forces ----------------------------------------------------------


def load_diagnostics(path: Path) -> list[dict[str, Any]]:
    by_step: dict[int, dict[str, Any]] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if line:
            record = json.loads(line)
            if "step" not in record:  # build-provenance meta line, not a step
                continue
            by_step[int(record["step"])] = record
    return [by_step[s] for s in sorted(by_step)]


def force_stats(records: list[dict[str, Any]]) -> ForceStats:
    cd = [float(r["cd"]) for r in records]
    cl = [float(r["cl"]) for r in records]

    def mean_std(values: list[float]) -> tuple[float, float]:
        m = sum(values) / len(values)
        return m, math.sqrt(sum((v - m) ** 2 for v in values) / len(values))

    cd_m, cd_s = mean_std(cd)
    cl_m, cl_s = mean_std(cl)
    return ForceStats(
        samples=len(records),
        t0=float(records[0]["t"]),
        t1=float(records[-1]["t"]),
        cd_mean=cd_m,
        cd_std=cd_s,
        cl_mean=cl_m,
        cl_std=cl_s,
        cd_min=min(cd),
        cd_max=max(cd),
        cl_min=min(cl),
        cl_max=max(cl),
    )


# --- spectrum (detrended Hann DFT, mirrors validation/postproc/spectra.py) ----------


# A reported Strouhal peak must (a) stand clearly above the broadband background
# and (b) be resolved by enough shedding cycles in the statistics window.
SNR_MIN = 5.0      # peak / background-mean ratio
SIGMA_MIN = 4.0    # peak must exceed background mean + SIGMA_MIN * background std
MIN_CYCLES = 5.0   # the window must span at least this many periods of the peak


def lift_spectrum(t: list[float], cl: list[float]) -> Spectrum:
    n = len(t)
    if n < 8:
        return Spectrum(
            st=[], mag=[], strouhal=None, frequency=None, prominent=False,
            resolution_st=0.0, snr=0.0, cycles=0.0, min_cycles=MIN_CYCLES,
            window_too_short=True,
            reason="window too short — not enough samples for a PSD",
        )
    dt = (t[-1] - t[0]) / (n - 1)
    samples: list[float] = []
    j = 0
    for i in range(n):
        ti = t[0] + i * dt
        while j + 1 < n - 1 and t[j + 1] < ti:
            j += 1
        ta, tb, ya, yb = t[j], t[j + 1], cl[j], cl[j + 1]
        wgt = 0.0 if tb == ta else (ti - ta) / (tb - ta)
        samples.append(ya + wgt * (yb - ya))

    mean0 = sum(samples) / n
    tbar = (n - 1) / 2.0
    denom = sum((i - tbar) ** 2 for i in range(n))
    slope = sum((i - tbar) * samples[i] for i in range(n)) / denom if denom else 0.0
    detrended = [samples[i] - (mean0 + slope * (i - tbar)) for i in range(n)]
    hann = [0.5 * (1.0 - math.cos(2.0 * math.pi * i / (n - 1))) for i in range(n)]
    x = [detrended[i] * hann[i] for i in range(n)]

    freqs: list[float] = []
    mags: list[float] = []
    for k in range(n // 2 + 1):
        re_part = im_part = 0.0
        for i in range(n):
            angle = -2.0 * math.pi * k * i / n
            re_part += x[i] * math.cos(angle)
            im_part += x[i] * math.sin(angle)
        freqs.append(k / (n * dt))
        mags.append(math.hypot(re_part, im_part) * 2.0 / n)

    resolution = freqs[1] - freqs[0] if len(freqs) > 1 else 0.0
    span = t[-1] - t[0]
    candidates = list(range(2, len(mags)))
    if not candidates:
        return Spectrum(
            st=freqs, mag=mags, strouhal=None, frequency=None, prominent=False,
            resolution_st=resolution, snr=0.0, cycles=0.0, min_cycles=MIN_CYCLES,
            window_too_short=True, reason="window too short — spectrum has no resolvable bins",
        )

    k_peak = max(candidates, key=lambda k: mags[k])
    f_peak = freqs[k_peak]
    others = [mags[k] for k in candidates if abs(k - k_peak) > 1]
    mean_o = sum(others) / len(others) if others else 0.0
    std_o = math.sqrt(sum((v - mean_o) ** 2 for v in others) / len(others)) if others else 0.0
    snr = mags[k_peak] / (mean_o + 1e-300)
    cycles = f_peak * span  # how many periods of the candidate fit the window

    # Significance test: clearly above the broadband floor AND resolved by enough
    # shedding cycles. Each clause yields an explicit, honest reason.
    above_floor = mags[k_peak] > 1e-9 * (1.0 + abs(mean0))
    significant = mags[k_peak] > mean_o + SIGMA_MIN * std_o and snr >= SNR_MIN
    enough_cycles = cycles >= MIN_CYCLES
    window_too_short = above_floor and significant and not enough_cycles
    prominent = above_floor and significant and enough_cycles

    if prominent:
        reason = f"dominant peak at St={f_peak:.3f} (SNR {snr:.1f}, {cycles:.1f} cycles)"
    elif not above_floor:
        reason = "no dominant shedding peak — lift spectrum is flat (wake steady)"
    elif not significant:
        reason = (
            f"no dominant shedding peak — strongest bin SNR {snr:.1f} < {SNR_MIN:.0f} "
            "above the broadband background (wake steady)"
        )
    else:  # significant but window too short
        reason = (
            f"candidate peak at St={f_peak:.3f} but window too short — only "
            f"{cycles:.1f} shedding cycles < {MIN_CYCLES:.0f} required; run longer "
            "or widen the statistics window"
        )

    return Spectrum(
        st=freqs,  # h = U = 1 in solver units, so f == St
        mag=mags,
        strouhal=f_peak if prominent else None,
        frequency=f_peak if prominent else None,
        prominent=prominent,
        resolution_st=resolution,
        snr=snr,
        cycles=cycles,
        min_cycles=MIN_CYCLES,
        window_too_short=window_too_short,
        reason=reason,
    )


# --- recirculation + vortex cores ---------------------------------------------------


def _mean_field(snapshots: list[Path]) -> UniformField:
    grid = parse_uniform_vtu(snapshots[0])
    n = grid.nx * grid.ny * grid.nz
    for path in snapshots[1:]:
        other = parse_uniform_vtu(path)
        for name in ("u", "v", "w", "p"):
            if name in grid.arrays and name in other.arrays:
                target = grid.arrays[name]
                src = other.arrays[name]
                for c in range(n):
                    target[c] += src[c]
    inv = 1.0 / len(snapshots)
    for name in ("u", "v", "w", "p"):
        if name in grid.arrays:
            grid.arrays[name] = [v * inv for v in grid.arrays[name]]
    return grid


def _solid_bbox(grid: UniformField) -> tuple[float, float, float, float, float, float] | None:
    solid = grid.arrays.get("solid")
    if not solid:
        return None
    lo = [math.inf] * 3
    hi = [-math.inf] * 3
    found = False
    for k in range(grid.nz):
        for j in range(grid.ny):
            for i in range(grid.nx):
                if solid[grid.idx(i, j, k)] > 0.5:
                    found = True
                    cx = grid.x0 + (i + 0.5) * grid.dx
                    cy = grid.y0 + (j + 0.5) * grid.dy
                    cz = grid.z0 + (k + 0.5) * grid.dz
                    for axis, (c, h) in enumerate(
                        ((cx, grid.dx / 2), (cy, grid.dy / 2), (cz, grid.dz / 2))
                    ):
                        lo[axis] = min(lo[axis], c - h)
                        hi[axis] = max(hi[axis], c + h)
    if not found:
        return None
    return (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2])


def _core_in_region(
    grid: UniformField,
    q: list[float],
    x_range: tuple[float, float],
    y_range: tuple[float, float],
    label: str,
) -> CoreMarker | None:
    best: CoreMarker | None = None
    solid = grid.arrays.get("solid", [])
    for k in range(grid.nz):
        for j in range(grid.ny):
            for i in range(grid.nx):
                c = grid.idx(i, j, k)
                if solid and solid[c] > 0.5:
                    continue
                x = grid.x0 + (i + 0.5) * grid.dx
                y = grid.y0 + (j + 0.5) * grid.dy
                if not (x_range[0] <= x <= x_range[1] and y_range[0] <= y <= y_range[1]):
                    continue
                if best is None or q[c] > best.q:
                    z = grid.z0 + (k + 0.5) * grid.dz
                    best = CoreMarker(label=label, x=x, y=y, z=z, q=q[c])
    if best is not None and best.q <= 0.0:
        return None
    return best


def recirculation_analysis(snapshots: list[Path], nu: float) -> Recirculation | None:
    if not snapshots:
        return None
    grid = _mean_field(snapshots)
    if "u" not in grid.arrays:
        return None
    cube = _solid_bbox(grid)
    if cube is None:
        return None
    u = grid.arrays["u"]
    solid = grid.arrays.get("solid", [0.0] * (grid.nx * grid.ny * grid.nz))
    h = cube[3] - cube[2] or 1.0
    z_mid = (cube[4] + cube[5]) / 2
    k_row = min(
        range(grid.nz), key=lambda k: abs(grid.z0 + (k + 0.5) * grid.dz - z_mid)
    )

    # floor shear sign changes along the centerline
    line: list[tuple[float, float]] = []
    for i in range(grid.nx):
        c = grid.idx(i, 0, k_row)
        if solid[c] > 0.5:
            continue
        x = grid.x0 + (i + 0.5) * grid.dx
        line.append((x, nu * u[c] / (grid.dy / 2)))
    points: list[CriticalPoint] = []
    for (x1, t1), (x2, t2) in itertools.pairwise(line):
        if x2 - x1 > 3.0 * grid.dx or t1 == 0.0 or t1 * t2 >= 0.0:
            continue
        xc = x1 - t1 * (x2 - x1) / (t2 - t1)
        points.append(CriticalPoint(x=xc, kind="separation" if t1 > 0 else "attachment"))

    x_front, x_rear = cube[0], cube[1]
    seps = [p.x for p in points if p.kind == "separation" and x_front - 3 * h <= p.x < x_front]
    atts = [p.x for p in points if p.kind == "attachment" and p.x > x_rear]
    x_sep = min(seps) if seps else None
    x_att = min(atts) if atts else None

    cell_vol = grid.dx * grid.dy * grid.dz
    reverse = sum(cell_vol for c in range(len(u)) if solid[c] < 0.5 and u[c] < 0.0) / h**3

    add_derived_fields(grid)
    q = grid.arrays.get("q", [])
    cores: list[CoreMarker] = []
    if q:
        horseshoe = _core_in_region(
            grid, q, (x_front - 1.5 * h, x_front), (0.0, 0.5 * h), "horseshoe vortex core"
        )
        arch = _core_in_region(
            grid, q, (x_rear, x_rear + 2.0 * h), (0.2 * h, 1.3 * h), "arch vortex core"
        )
        cores = [m for m in (horseshoe, arch) if m is not None]

    return Recirculation(
        x_separation=x_sep,
        x_reattachment=x_att,
        upstream_separation_over_h=(x_front - x_sep) / h if x_sep is not None else None,
        reattachment_over_h=(x_att - x_rear) / h if x_att is not None else None,
        reverse_flow_volume_over_h3=reverse,
        critical_points=points,
        cores=cores,
    )


# --- reference comparison (validation registry) -------------------------------------

BANDS: dict[str, tuple[float, float]] = {
    "st": (0.05, 0.10),
    "cd_mean": (0.05, 0.10),
    "cl_mean": (0.05, 0.10),
}
DEFAULT_BAND = (0.10, 0.20)


def load_reference(
    registry: Path, reynolds: float
) -> tuple[int | None, dict[str, tuple[float | None, str]]]:
    """Reference metrics for the nearest available Re rung."""
    if not registry.is_dir():
        return None, {}
    rungs = []
    for child in registry.iterdir():
        m = re.fullmatch(r"re(\d+)", child.name)
        if m and (child / "metrics.csv").is_file():
            rungs.append((int(m.group(1)), child / "metrics.csv"))
    if not rungs:
        return None, {}
    re_value, csv_path = min(rungs, key=lambda item: abs(item[0] - reynolds))
    out: dict[str, tuple[float | None, str]] = {}
    lines = csv_path.read_text().splitlines()
    for line in lines[1:]:
        parts = line.split(",")
        if len(parts) < 5 or parts[0].startswith("#"):
            continue
        try:
            value: float | None = float(parts[1])
        except ValueError:
            value = None
        out[parts[0]] = (value, parts[4])
    return re_value, out


def compare(
    computed: dict[str, float | None], reference: dict[str, tuple[float | None, str]]
) -> list[ComparisonRow]:
    rows = []
    for metric, c in computed.items():
        ref_value, source = reference.get(metric, (None, ""))
        band = BANDS.get(metric, DEFAULT_BAND)
        if c is None:
            status, err = "N/A", None
        elif ref_value is None:
            status, err = "NO-REF (TODO)", None
        else:
            err = abs(c - ref_value) / abs(ref_value) if ref_value else abs(c)
            status = "PASS" if err <= band[0] else "MARGINAL" if err <= band[1] else "FAIL"
        rows.append(
            ComparisonRow(
                metric=metric, computed=c, reference=ref_value, rel_error=err,
                band=band, status=status, source=source,
            )
        )
    return rows


# --- audit summary -------------------------------------------------------------------

_ACC_RE = re.compile(r"(\d+) accepted, (\d+) rejected")
_GUARD_RE = re.compile(r"(\d+) reduced proposals")


def audit_summary(workspace: Path) -> AuditSummary | None:
    audit_path = workspace / "audit.jsonl"
    if not audit_path.is_file():
        return None
    accepts = rejects = guards = refinements = coarsenings = 0
    for line in audit_path.read_text().splitlines():
        if not line.strip():
            continue
        event = json.loads(line)
        kind = event.get("event")
        if kind == "refine":
            refinements += int(event.get("count", 0))
        elif kind == "coarsen":
            coarsenings += int(event.get("count", 0))
        elif kind == "acceptance":
            reason = event.get("reason", "")
            if event.get("region") == "hard-guard-zones":
                m = _GUARD_RE.search(reason)
                if m:
                    guards += int(m.group(1))
            else:
                m = _ACC_RE.search(reason)
                if m:
                    accepts += int(m.group(1))
                    rejects += int(m.group(2))

    speedup = mean_cells = fine_cells = None
    efficiency = workspace / "efficiency.txt"
    if efficiency.is_file():
        text = efficiency.read_text()
        for key, pattern in (
            ("speedup", r"cell speedup = ([\d.]+)x"),
            ("mean", r"adaptive mean cells = ([\d.]+)"),
            ("fine", r"uniform-fine cells = ([\d.]+)"),
        ):
            m = re.search(pattern, text)
            if m:
                value = float(m.group(1))
                if key == "speedup":
                    speedup = value
                elif key == "mean":
                    mean_cells = value
                else:
                    fine_cells = value

    return AuditSummary(
        accepts=accepts,
        rejects=rejects,
        promotions=rejects,
        guard_overrides=guards,
        refinements=refinements,
        coarsenings=coarsenings,
        cell_speedup=speedup,
        adaptive_mean_cells=mean_cells,
        uniform_fine_cells=fine_cells,
    )


# --- top-level -------------------------------------------------------------------------


def run_analysis(
    workspace: Path,
    reynolds: float,
    registry: Path,
    window_frac: float = 0.5,
    max_mean_snapshots: int = 4,
) -> AnalysisResponse:
    warnings: list[str] = []

    forces = None
    spectrum = None
    diag_path = workspace / "diagnostics.jsonl"
    if diag_path.is_file():
        records = load_diagnostics(diag_path)
        last = records[-1]["step"] if records else 0
        window = [r for r in records if r["step"] >= last * (1.0 - window_frac)]
        if window:
            forces = force_stats(window)
            spectrum = lift_spectrum(
                [float(r["t"]) for r in window], [float(r["cl"]) for r in window]
            )
            if float(records[-1]["momentum_residual"]) > 1e-2:
                warnings.append(
                    f"momentum residual still {records[-1]['momentum_residual']:.1e} at the last "
                    "step — force statistics may not be converged"
                )
        if spectrum is not None and not spectrum.prominent:
            warnings.append(f"Strouhal: {spectrum.reason}")
            if spectrum.window_too_short:
                warnings.append(
                    "lengthen the run or widen the statistics window to resolve the candidate "
                    "shedding peak"
                )
            elif reynolds <= 1000:
                warnings.append(
                    f"a non-detection at Re_h={reynolds:.0f} is physically expected — the "
                    "wall-mounted-cube wake is steady or only weakly unsteady here; clean shedding "
                    "peaks appear higher up the Reynolds ladder"
                )

    snapshots = sorted(
        workspace.glob("snapshot_*.vtu"),
        key=lambda p: int(re.findall(r"(\d+)", p.name)[-1]),
    )[-max_mean_snapshots:]
    recirculation = None
    try:
        recirculation = recirculation_analysis(snapshots, nu=1.0 / max(reynolds, 1e-9))
    except ValueError:
        warnings.append("snapshots are not on a uniform grid — recirculation analysis skipped")
    if recirculation is not None and recirculation.x_reattachment is None:
        warnings.append("floor reattachment point not detected in the statistics window")

    computed: dict[str, float | None] = {
        "cd_mean": forces.cd_mean if forces else None,
        "cl_mean": forces.cl_mean if forces else None,
        "st": spectrum.strouhal if spectrum else None,
        "upstream_separation_over_h": (
            recirculation.upstream_separation_over_h if recirculation else None
        ),
        "reattachment_over_h": recirculation.reattachment_over_h if recirculation else None,
        "reverse_flow_volume_over_h3": (
            recirculation.reverse_flow_volume_over_h3 if recirculation else None
        ),
    }
    reference_re, reference = load_reference(registry, reynolds)
    comparison = compare(computed, reference)
    if reference_re is not None and all(row.reference is None for row in comparison):
        warnings.append(
            f"reference registry rung re{reference_re:04d} exists but all values are TODO — "
            "digitize the literature values to activate pass/fail"
        )

    audit = audit_summary(workspace)
    if audit is None:
        warnings.append(
            "no audit trail (uniform run) — launch with adaptive=true for audited model decisions"
        )

    return AnalysisResponse(
        window_frac=window_frac,
        forces=forces,
        spectrum=spectrum,
        recirculation=recirculation,
        comparison=comparison,
        audit=audit,
        warnings=warnings,
        reference_re=reference_re,
    )
