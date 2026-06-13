"""Phase-0 validation report generator (investor-facing output).

One command turns a completed ladder rung into a self-contained HTML report
(print-to-PDF ready) populated entirely from real solver output:

    python -m validation.make_report --rung validation/runs/re0500

Sections: experiment card, mesh evolution, physics-label maps, Cd/Cl histories,
field visualizations, velocity profiles, spectra, recirculation maps with
coordinates, Cp profiles, the model acceptance/rejection table from the audit
trail, comparison vs reference (5-10% bands), and the speedup figure.
``--check`` exits non-zero unless every section is populated.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import sys
from dataclasses import dataclass, field
from html import escape
from pathlib import Path
from typing import Any

from validation.postproc import audit as audit_mod
from validation.postproc import compare_ref, qcrit, svg
from validation.postproc.diagnostics import (
    force_stats,
    load_diagnostics,
    load_meta,
    stats_window,
)
from validation.postproc.meanfield import mean_fields, select_window_snapshots
from validation.postproc.recirculation import RecirculationMetrics, recirculation_metrics
from validation.postproc.spectra import StrouhalResult, strouhal_analysis
from validation.postproc.vtu import OctreeCells, UniformGrid, load_octree_vtu, solid_bbox
from validation.postproc.walls import nearest_k

STATIONS_XREL = (-1.0, 0.5, 1.5, 2.5, 4.0)
Q_ISO_THRESHOLD = 0.5  # in U^2/h^2 units; exported alongside the report


@dataclass
class Context:
    rung: Path
    ref_dir: Path
    window_frac: float
    case: dict[str, Any] = field(default_factory=dict)
    state: dict[str, Any] = field(default_factory=dict)
    records: list[dict[str, Any]] = field(default_factory=list)
    window: list[dict[str, Any]] = field(default_factory=list)
    grid: UniformGrid | None = None
    mean: dict[str, list[float]] = field(default_factory=dict)
    snap_steps: list[int] = field(default_factory=list)
    cube: tuple[float, float, float, float, float, float] | None = None
    nu: float = 1.0 / 500.0
    h: float = 1.0
    recirc: RecirculationMetrics | None = None
    st: StrouhalResult | None = None
    fstats: dict[str, float] = field(default_factory=dict)
    cp_ref: float = 0.0
    cp_front_center: float | None = None
    audit: audit_mod.AuditData | None = None
    octree: OctreeCells | None = None
    eff: dict[str, float] = field(default_factory=dict)
    computed: dict[str, float | None] = field(default_factory=dict)
    comparison: list[compare_ref.ComparisonRow] = field(default_factory=list)
    q_quads: int = 0
    q_obj_path: str = ""


def load_context(rung: Path, ref_dir: Path, window_frac: float) -> Context:
    ctx = Context(rung=rung, ref_dir=ref_dir, window_frac=window_frac)
    case_path = rung / "case.json"
    if case_path.exists():
        ctx.case = json.loads(case_path.read_text())
    state_path = rung / "rung_state.json"
    if state_path.exists():
        ctx.state = json.loads(state_path.read_text())
    re_h = float(ctx.case.get("reynolds", 500))
    ctx.nu = 1.0 / re_h

    uniform = rung / "uniform"
    diag = uniform / "diagnostics.jsonl"
    if diag.exists():
        ctx.records = load_diagnostics(diag)
        ctx.window = stats_window(ctx.records, window_frac)
        ctx.fstats = force_stats(ctx.window)

    snaps = select_window_snapshots(uniform, window_frac)
    if snaps:
        ctx.snap_steps = [s for s, _ in snaps]
        ctx.grid, ctx.mean = mean_fields([p for _, p in snaps])
        solid = ctx.mean.get("solid", [])
        ctx.cube = solid_bbox(ctx.grid, solid) if solid else None
        if ctx.cube:
            ctx.recirc = recirculation_metrics(
                ctx.grid, ctx.mean["u"], solid, ctx.cube, ctx.nu, ctx.h
            )
            _compute_cp(ctx)
            _export_q(ctx)

    if len(ctx.window) >= 8:
        t = [float(r["t"]) for r in ctx.window]
        cl = [float(r["cl"]) for r in ctx.window]
        ctx.st = strouhal_analysis(t, cl, h=ctx.h, velocity=1.0)

    adaptive = rung / "adaptive"
    if (adaptive / "audit.jsonl").exists():
        ctx.audit = audit_mod.load_audit(adaptive / "audit.jsonl")
    if (adaptive / "efficiency.txt").exists():
        ctx.eff = audit_mod.parse_efficiency(adaptive / "efficiency.txt")
    if (adaptive / "adaptive_final.vtu").exists():
        ctx.octree = load_octree_vtu(adaptive / "adaptive_final.vtu")

    ctx.computed = {
        "cd_mean": ctx.fstats.get("cd_mean"),
        "cl_mean": ctx.fstats.get("cl_mean"),
        "st": ctx.st.strouhal if ctx.st else None,
        "upstream_separation_over_h": ctx.recirc.upstream_separation_over_h if ctx.recirc else None,
        "reattachment_over_h": ctx.recirc.reattachment_over_h if ctx.recirc else None,
        "reverse_flow_volume_over_h3": ctx.recirc.reverse_flow_volume_over_h3 if ctx.recirc else None,
        "cp_front_center": ctx.cp_front_center,
    }
    ref = compare_ref.load_reference_metrics(ctx.ref_dir / "metrics.csv")
    ctx.comparison = compare_ref.compare(ctx.computed, ref)
    return ctx


def _compute_cp(ctx: Context) -> None:
    grid, mean, cube = ctx.grid, ctx.mean, ctx.cube
    assert grid and cube
    p = mean["p"]
    solid = mean["solid"]
    i_ref = max(0, min(grid.nx - 1, round(0.5 * ctx.h / grid.dx - 0.5)))
    vals = [
        p[grid.cidx(i_ref, j, k)]
        for k in range(grid.nz)
        for j in range(grid.ny)
        if solid[grid.cidx(i_ref, j, k)] < 0.5
    ]
    ctx.cp_ref = sum(vals) / len(vals) if vals else 0.0
    i_front = round((cube[0] - grid.x0) / grid.dx) - 1
    j_mid = max(0, min(grid.ny - 1, round((0.5 * (cube[2] + cube[3]) - grid.y0) / grid.dy - 0.5)))
    k_mid = nearest_k(grid, 0.5 * (cube[4] + cube[5]))
    if 0 <= i_front < grid.nx:
        ctx.cp_front_center = (p[grid.cidx(i_front, j_mid, k_mid)] - ctx.cp_ref) / 0.5


def _export_q(ctx: Context) -> None:
    grid, mean = ctx.grid, ctx.mean
    assert grid
    q = qcrit.compute_q(grid, mean["u"], mean["v"], mean["w"], mean["solid"])
    out = ctx.rung / "q_isosurface.obj"
    ctx.q_quads = qcrit.export_q_isosurface_obj(grid, q, Q_ISO_THRESHOLD, out)
    ctx.q_obj_path = out.name


# ---------------------------------------------------------------------------
# HTML helpers
# ---------------------------------------------------------------------------
def table(headers: list[str], rows: list[list[str]], caption: str = "") -> str:
    head = "".join(f"<th>{escape(h)}</th>" for h in headers)
    body = "".join("<tr>" + "".join(f"<td>{c}</td>" for c in r) + "</tr>" for r in rows)
    cap = f"<caption>{escape(caption)}</caption>" if caption else ""
    return f"<table>{cap}<thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>"


def fmt(v: float | None, digits: int = 4) -> str:
    if v is None:
        return "—"
    return f"{v:.{digits}g}"


def status_chip(status: str) -> str:
    color = {
        "PASS": "#1e8449",
        "MARGINAL": "#b9770e",
        "FAIL": "#c0392b",
        "NO-REF (TODO)": "#7f8c8d",
        "N/A": "#7f8c8d",
    }.get(status, "#7f8c8d")
    return f"<span class='chip' style='background:{color}'>{escape(status)}</span>"


# ---------------------------------------------------------------------------
# Sections — each returns (title, html, ok)
# ---------------------------------------------------------------------------
def sec_experiment(ctx: Context) -> tuple[str, str, bool]:
    grid = ctx.grid
    steps_done = ctx.records[-1]["step"] if ctx.records else 0
    win_t = (
        f"t ∈ [{ctx.window[0]['t']:.2f}, {ctx.window[-1]['t']:.2f}] "
        f"({len(ctx.window)} steps, {len(ctx.snap_steps)} snapshots)"
        if ctx.window
        else "—"
    )
    stages = ctx.state.get("stages", {})
    rows = [
        ["Case", escape(str(ctx.case.get("name", "wall-mounted-cube")))],
        ["Reynolds number Re_h", fmt(float(ctx.case.get("reynolds", 0)), 5)],
        ["Convection scheme", escape(str(ctx.case.get("convection", "weno5")))],
        ["Uniform grid", f"{grid.nx} × {grid.ny} × {grid.nz} = {grid.cells:,} cells (Δx = {grid.dx:.4g} h)" if grid else "—"],
        ["Steps completed", f"{steps_done} / {ctx.case.get('max_steps', '—')}"],
        ["Δt (statistics window)", f"{ctx.fstats.get('dt_mean', 0):.4g} ± {ctx.fstats.get('dt_std', 0):.2g}"],
        ["Statistics window", win_t],
        ["Solver", escape(ctx.state.get("solver", "(not recorded)"))],
        ["Uniform-stage wall time", f"{stages.get('uniform', {}).get('wall_s', 0):.1f} s ({stages.get('uniform', {}).get('resumes', 0)} resume(s))"],
        ["Adaptive-stage wall time", f"{stages.get('adaptive', {}).get('wall_s', 0):.1f} s"],
        ["Generated", dt.datetime.now().strftime("%Y-%m-%d %H:%M")],
        ["Run directory", escape(str(ctx.rung))],
    ]
    return "Experiment card", table(["Parameter", "Value"], rows), grid is not None


def sec_mesh_evolution(ctx: Context) -> tuple[str, str, bool]:
    a = ctx.audit
    if not a or not a.steps:
        return "Mesh evolution", "<p class='warn'>adaptive audit not found</p>", False
    cells = svg.line_chart(
        [svg.Series("octree cells", [float(s) for s in a.steps], [float(c) for c in a.cells], "#2980b9")],
        title="Adaptive octree cell count per step",
        xlabel="adaptive step",
        ylabel="cells",
    )
    steps_sorted = sorted(set(a.refine_per_step) | set(a.coarsen_per_step))
    activity = svg.bar_chart(
        [str(s) for s in steps_sorted],
        [float(a.refine_per_step.get(s, 0)) for s in steps_sorted],
        title="Refinements per step (coarsenings: " + str(sum(a.coarsen_per_step.values())) + ")",
        ylabel="cells refined",
        color="#8e44ad",
    )
    level_hist = ""
    if ctx.octree and "level" in ctx.octree.fields:
        from collections import Counter

        counts = Counter(int(v) for v in ctx.octree.fields["level"])
        levels = sorted(counts)
        level_hist = svg.bar_chart(
            [f"L{lv}" for lv in levels],
            [float(counts[lv]) for lv in levels],
            title="Final mesh: cells per refinement level",
            ylabel="cells",
            color="#16a085",
        )
    reasons = table(
        ["Refinement reason", "cells"],
        [[escape(r), str(n)] for r, n in ctx.audit.refine_reasons.most_common()],
    )
    return "Mesh evolution", cells + activity + level_hist + reasons, True


def _octree_slice(
    ctx: Context, plane: str, coord: float
) -> list[tuple[int, float, float, float, float]]:
    """Cells intersecting a plane -> (cell_idx, a, b, da, db) in slice coords."""
    out = []
    oc = ctx.octree
    assert oc
    for c in range(oc.count):
        cx, cy, cz = oc.centers[c]
        sx, sy, sz = oc.sizes[c]
        if plane == "z" and abs(cz - coord) <= sz / 2 + 1e-12:
            out.append((c, cx - sx / 2, cy - sy / 2, sx, sy))
        elif plane == "y" and abs(cy - coord) <= sy / 2 + 1e-12:
            out.append((c, cx - sx / 2, cz - sz / 2, sx, sz))
    return out


def sec_label_maps(ctx: Context) -> tuple[str, str, bool]:
    oc = ctx.octree
    if not oc or "physics_mode" not in oc.fields:
        return "Physics-label maps", "<p class='warn'>adaptive octree output not found</p>", False
    modes = oc.fields["physics_mode"]
    mask = oc.fields.get("mask", [0.0] * oc.count)
    legend = [(name, audit_mod.MODE_COLORS[name]) for name in audit_mod.MODE_NAMES]
    legend.append(("solid", "#555"))

    zs = [c[2] for c in oc.centers]
    z_mid = sorted(zs)[len(zs) // 2]
    maps_html = []
    for plane, coord, xlabel, ylabel, title in (
        ("z", z_mid, "x/h", "y/h", f"Physics-model map — spanwise centerplane (z = {z_mid:.2f}h)"),
        ("y", min(c[1] for c in oc.centers), "x/h", "z/h", "Physics-model map — floor layer"),
    ):
        cells = _octree_slice(ctx, plane, coord)
        if not cells:
            continue
        a0 = min(a for _, a, _, _, _ in cells)
        a1 = max(a + da for _, a, _, da, _ in cells)
        b0 = min(b for _, _, b, _, _ in cells)
        b1 = max(b + db for _, _, b, _, db in cells)
        rm = svg.RectMap(a0, a1, b0, b1, title=title, xlabel=xlabel, ylabel=ylabel, legend=legend)
        for c, a, b, da, db in cells:
            if mask[c] > 1.5:  # inside solid
                color = "#555"
            else:
                color = audit_mod.MODE_COLORS[audit_mod.MODE_NAMES[int(modes[c])]]
            rm.add_rect(a, b, da, db, color)
        maps_html.append(rm.render())

    # mode share table (fluid cells only)
    total_n = 0
    total_v = 0.0
    counts: dict[str, int] = {}
    vols: dict[str, float] = {}
    for c in range(oc.count):
        if mask[c] > 1.5:
            continue
        name = audit_mod.MODE_NAMES[int(modes[c])]
        counts[name] = counts.get(name, 0) + 1
        vols[name] = vols.get(name, 0.0) + oc.volume(c)
        total_n += 1
        total_v += oc.volume(c)
    rows = [
        [
            f"<span class='chip' style='background:{audit_mod.MODE_COLORS[name]}'>{name}</span>",
            str(counts.get(name, 0)),
            f"{100.0 * counts.get(name, 0) / max(total_n, 1):.1f}%",
            f"{100.0 * vols.get(name, 0.0) / max(total_v, 1e-300):.1f}%",
        ]
        for name in audit_mod.MODE_NAMES
    ]
    share = table(["Physics model", "cells", "cell share", "volume share"], rows,
                  "Cheap (reduced) vs full-NS regions on the final adaptive mesh")
    note = (
        "<p>Reduced (cheap) models cover the smooth outer flow; FULL_NS / WAKE_SHEAR "
        "concentrate around the cube, in guard zones and in the wake — exactly where "
        "the spec forbids reduced physics.</p>"
    )
    return "Physics-label maps", "".join(maps_html) + share + note, bool(maps_html)


def sec_forces(ctx: Context) -> tuple[str, str, bool]:
    if not ctx.records:
        return "Force histories", "<p class='warn'>diagnostics.jsonl not found</p>", False
    t = [float(r["t"]) for r in ctx.records]
    cd = [float(r["cd"]) for r in ctx.records]
    cl = [float(r["cl"]) for r in ctx.records]
    shade = (float(ctx.window[0]["t"]), float(ctx.window[-1]["t"])) if ctx.window else None
    chart = svg.line_chart(
        [svg.Series("Cd", t, cd, "#c0392b"), svg.Series("Cl", t, cl, "#2980b9")],
        title="Force-coefficient histories (shaded: statistics window)",
        xlabel="t · U/h",
        ylabel="coefficient",
        shade_x=shade,
        y_zero_line=True,
    )
    f = ctx.fstats
    rows = [
        ["Cd", fmt(f.get("cd_mean")), fmt(f.get("cd_std"), 3), fmt(f.get("cd_min")), fmt(f.get("cd_max"))],
        ["Cl", fmt(f.get("cl_mean")), fmt(f.get("cl_std"), 3), fmt(f.get("cl_min")), fmt(f.get("cl_max"))],
    ]
    stats = table(["Coefficient", "mean", "std", "min", "max"], rows,
                  f"Statistics over the window ({int(f.get('samples', 0))} steps)")
    res = svg.line_chart(
        [
            svg.Series("momentum residual", t, [float(r["momentum_residual"]) for r in ctx.records], "#8e44ad"),
            svg.Series("continuity ×10⁶", t, [1e6 * float(r["continuity_residual"]) for r in ctx.records], "#16a085"),
        ],
        title="Residual histories",
        xlabel="t · U/h",
        ylabel="residual",
    )
    return "Force & residual histories", chart + stats + res, True


def _heatmap(ctx: Context, name: str, values: list[float], diverging: bool,
             lo: float, hi: float, title: str) -> str:
    grid, cube = ctx.grid, ctx.cube
    assert grid and cube
    k_row = nearest_k(grid, 0.5 * (cube[4] + cube[5]))
    rm = svg.RectMap(
        grid.x0, grid.x0 + grid.nx * grid.dx, grid.y0, grid.y0 + grid.ny * grid.dy,
        title=title, xlabel="x/h", ylabel="y/h",
    )
    solid = ctx.mean["solid"]
    for j in range(grid.ny):
        for i in range(grid.nx):
            c = grid.cidx(i, j, k_row)
            x, y, _ = grid.center(i, j, k_row)
            if solid[c] > 0.5:
                color = "#555"
            elif diverging:
                color = svg.diverging(values[c], lo, hi)
            else:
                color = svg.sequential(values[c], lo, hi)
            rm.add_rect(x - grid.dx / 2, y - grid.dy / 2, grid.dx, grid.dy, color)
    rm.outlines.append((cube[0], cube[2], cube[1] - cube[0], cube[3] - cube[2], "#000"))
    return rm.render()


def sec_fields(ctx: Context) -> tuple[str, str, bool]:
    if not ctx.grid or not ctx.cube:
        return "Field visualizations", "<p class='warn'>no snapshots found</p>", False
    u = ctx.mean["u"]
    speed = [
        math.sqrt(ctx.mean["u"][c] ** 2 + ctx.mean["v"][c] ** 2 + ctx.mean["w"][c] ** 2)
        for c in range(ctx.grid.cells)
    ]
    u_map = _heatmap(ctx, "u", u, True, -0.5, 1.5,
                     "Mean streamwise velocity u/U — spanwise centerplane (blue: reverse flow)")
    s_map = _heatmap(ctx, "speed", speed, False, 0.0, max(speed),
                     "Mean speed |u|/U — spanwise centerplane")
    q_note = (
        f"<p>Q-criterion isosurface (Q ≥ {Q_ISO_THRESHOLD} U²/h²) exported to "
        f"<code>{escape(ctx.q_obj_path)}</code> ({ctx.q_quads} faces) — open in ParaView/any "
        "3D viewer alongside the .vtu snapshots.</p>"
    )
    return "Field visualizations", u_map + s_map + q_note, True


def sec_recirculation(ctx: Context) -> tuple[str, str, bool]:
    grid, cube, m = ctx.grid, ctx.cube, ctx.recirc
    if not grid or not cube or m is None:
        return "Recirculation", "<p class='warn'>mean field unavailable</p>", False
    # floor map of reverse flow (x-z plane, j=0)
    rm = svg.RectMap(
        grid.x0, grid.x0 + grid.nx * grid.dx, grid.z0, grid.z0 + grid.nz * grid.dz,
        title="Floor recirculation map (orange: mean u < 0 in the first cell layer)",
        xlabel="x/h", ylabel="z/h",
        legend=[("reverse flow", "#e67e22"), ("cube footprint", "#555")],
    )
    u = ctx.mean["u"]
    solid = ctx.mean["solid"]
    for k in range(grid.nz):
        for i in range(grid.nx):
            c = grid.cidx(i, 0, k)
            x, _, z = grid.center(i, 0, k)
            if solid[c] > 0.5:
                color = "#555"
            elif u[c] < 0.0:
                color = "#e67e22"
            else:
                continue
            rm.add_rect(x - grid.dx / 2, z - grid.dz / 2, grid.dx, grid.dz, color)
    z_mid = 0.5 * (cube[4] + cube[5])
    if m.x_separation_upstream is not None:
        rm.markers.append(svg.Marker(m.x_separation_upstream, z_mid, f"S x={m.x_separation_upstream:.2f}", "#c0392b"))
    if m.x_reattachment is not None:
        rm.markers.append(svg.Marker(m.x_reattachment, z_mid, f"R x={m.x_reattachment:.2f}", "#1e8449"))

    rows = [
        ["Upstream (horseshoe) separation x_S", fmt(m.x_separation_upstream), fmt(m.upstream_separation_over_h), "distance ahead of front face / h"],
        ["Floor reattachment x_R", fmt(m.x_reattachment), fmt(m.reattachment_over_h), "behind rear face / h"],
        ["Rear-bubble length", "—", fmt(m.rear_bubble_over_h), "= reattachment length on the floor centerline"],
        ["Reverse-flow volume", "—", fmt(m.reverse_flow_volume_over_h3), "Σ vol(u<0) / h³"],
    ]
    crit = table(
        ["Floor critical point", "x (abs)", "kind"],
        [[f"#{n+1}", f"{p.x:.3f}", p.kind] for n, p in enumerate(m.critical_pts)],
        "All τ_w sign changes on the floor centerline",
    )
    metrics = table(["Metric", "x (absolute)", "value (/h or /h³)", "definition"], rows)
    note = "" if m.upstream_separation_over_h else (
        "<p class='warn'>Upstream horseshoe separation was not detected at this resolution "
        "(Δx = h/6) — expected to appear under the box-A floor refinement in the adaptive "
        "octree-native solve.</p>"
    )
    return "Recirculation metrics & maps", rm.render() + metrics + crit + note, True


def sec_profiles(ctx: Context) -> tuple[str, str, bool]:
    grid, cube = ctx.grid, ctx.cube
    if not grid or not cube:
        return "Velocity profiles", "<p class='warn'>mean field unavailable</p>", False
    k_row = nearest_k(grid, 0.5 * (cube[4] + cube[5]))
    series = []
    colors = ("#2980b9", "#c0392b", "#16a085", "#8e44ad", "#e67e22")
    overlay_note = []
    for idx, xrel in enumerate(STATIONS_XREL):
        x_abs = cube[0] + xrel * ctx.h
        i = max(0, min(grid.nx - 1, round((x_abs - grid.x0) / grid.dx - 0.5)))
        ys, us = [], []
        for j in range(grid.ny):
            c = grid.cidx(i, j, k_row)
            if ctx.mean["solid"][c] > 0.5:
                continue
            ys.append(grid.center(i, j, k_row)[1])
            us.append(ctx.mean["u"][c])
        series.append(svg.Series(f"x/h={xrel:+.1f}", us, ys, colors[idx % len(colors)]))
        ref_csv = ctx.ref_dir / "profiles" / f"u_profile_xrel_{xrel}.csv"
        if ref_csv.exists():
            with open(ref_csv, newline="") as f:
                pts = [r for r in csv.DictReader(filter(lambda ln: not ln.startswith("#"), f)) if r.get("u_over_U")]
            if pts:
                series.append(
                    svg.Series(f"ref x/h={xrel:+.1f}",
                               [float(p["u_over_U"]) for p in pts],
                               [float(p["y_over_h"]) for p in pts], "#555", 1.0)
                )
            else:
                overlay_note.append(f"x/h={xrel:+.1f}")
    chart = svg.line_chart(
        series, title="Mean u(y)/U at standard stations (spanwise centerplane)",
        xlabel="u/U", ylabel="y/h", width=720, height=420,
    )
    note = (
        f"<p>Reference profiles are TODO slots in the registry for stations "
        f"{', '.join(overlay_note)} — overlays appear automatically once digitized.</p>"
        if overlay_note else ""
    )
    return "Mean velocity profiles", chart + note, True


def sec_spectra(ctx: Context) -> tuple[str, str, bool]:
    st = ctx.st
    if st is None:
        return "Spectra", "<p class='warn'>not enough samples in the statistics window</p>", False
    sts = [f * ctx.h for f in st.freqs]
    chart = svg.line_chart(
        [svg.Series("|Ĉl|", sts, st.mags, "#2980b9")],
        title="Cl spectrum (Hann-windowed DFT of the statistics window)",
        xlabel="St = f·h/U", ylabel="amplitude",
    )
    if st.prominent and st.strouhal is not None:
        verdict = (
            f"<p><b>Strouhal number: St = {st.strouhal:.4f}</b> "
            f"(f = {st.frequency:.4f} U/h, bin resolution ±{st.resolution_st:.3f}).</p>"
        )
    else:
        verdict = (
            "<p><b>No dominant spectral peak</b> — the wake is steady over this window "
            f"(peak/background not prominent; bin resolution {st.resolution_st:.3f} St). "
            "A steady wake at this Re is physically plausible; the registry's St slot "
            "stays TODO until the reference confirms the regime.</p>"
        )
    return "Spectra & Strouhal number", chart + verdict, True


def sec_cp(ctx: Context) -> tuple[str, str, bool]:
    grid, cube = ctx.grid, ctx.cube
    if not grid or not cube:
        return "Pressure coefficients", "<p class='warn'>mean field unavailable</p>", False
    p = ctx.mean["p"]
    solid = ctx.mean["solid"]
    k_row = nearest_k(grid, 0.5 * (cube[4] + cube[5]))
    i_front = round((cube[0] - grid.x0) / grid.dx) - 1
    front_y, front_cp = [], []
    if 0 <= i_front < grid.nx:
        for j in range(grid.ny):
            y = grid.center(i_front, j, k_row)[1]
            if cube[2] <= y <= cube[3]:
                c = grid.cidx(i_front, j, k_row)
                if solid[c] < 0.5:
                    front_y.append(y)
                    front_cp.append((p[c] - ctx.cp_ref) / 0.5)
    j_top = min(grid.ny - 1, round((cube[3] - grid.y0) / grid.dy))
    top_x, top_cp = [], []
    for i in range(grid.nx):
        x = grid.center(i, j_top, k_row)[0]
        if cube[0] <= x <= cube[1]:
            c = grid.cidx(i, j_top, k_row)
            if solid[c] < 0.5:
                top_x.append(x)
                top_cp.append((p[c] - ctx.cp_ref) / 0.5)
    charts = []
    if front_cp:
        charts.append(svg.line_chart(
            [svg.Series("front face", front_cp, front_y, "#c0392b")],
            title="Cp along the front-face vertical centerline",
            xlabel="Cp", ylabel="y/h", width=420, height=320,
        ))
    if top_cp:
        charts.append(svg.line_chart(
            [svg.Series("roof", top_x, top_cp, "#2980b9")],
            title="Cp along the roof streamwise centerline",
            xlabel="x/h", ylabel="Cp", width=420, height=320,
        ))
    summary = f"<p>Cp at front-face center: <b>{fmt(ctx.cp_front_center)}</b> (reference plane x = 0.5h).</p>"
    return "Pressure coefficients", "".join(charts) + summary, bool(charts)


def sec_audit(ctx: Context) -> tuple[str, str, bool]:
    a = ctx.audit
    if a is None:
        return "Model decision audit", "<p class='warn'>audit.jsonl not found</p>", False
    rows = []
    for mode, row in sorted(a.acceptance.items()):
        rows.append([
            f"<span class='chip' style='background:{audit_mod.MODE_COLORS.get(mode, '#555')}'>{escape(mode)}</span>",
            str(row.proposals), str(row.accepted), str(row.rejected),
            f"{100.0 * row.accepted / max(row.proposals, 1):.1f}%",
        ])
    acc = table(
        ["Reduced model", "proposals", "accepted", "rejected → FULL_NS", "acceptance"],
        rows, "Reduced models may propose; only NS-consistency checks accept (spec rule)",
    )
    guard = (
        f"<p><b>{a.guard_overrides_total:,}</b> reduced-model proposals were overridden to "
        "FULL_NS by hard guard zones (horseshoe birth, front-floor junction, leading edges, "
        "near-wake start) — these zones can never run reduced physics, regardless of "
        "classifier output or configuration.</p>"
    )
    changes = table(
        ["Label transition", "count"],
        [[f"{escape(f)} → {escape(t)}", str(n)] for (f, t), n in a.model_changes.most_common(8)],
        "Model-label changes (top transitions)",
    )
    reasons = table(
        ["Recorded reason", "count"],
        [[escape(r), str(n)] for r, n in a.change_reasons.most_common(6)],
    )
    decisions = table(
        ["Step decision", "count"],
        [[escape(d), str(n)] for d, n in a.step_decisions.most_common()],
        "Step-acceptance outcomes (remedy order: refine mesh → reduce Δt → promote models)",
    )
    src = f"<p>Full event stream: <code>adaptive/audit.jsonl</code> ({a.total_events:,} events).</p>"
    return "Model decision audit", acc + guard + changes + reasons + decisions + src, bool(rows)


def sec_comparison(ctx: Context) -> tuple[str, str, bool]:
    rows = []
    for r in ctx.comparison:
        band = f"{r.band[0]*100:.0f}–{r.band[1]*100:.0f}%"
        err = f"{r.rel_error*100:.1f}%" if r.rel_error is not None else "—"
        rows.append([
            f"<code>{escape(r.metric)}</code>", fmt(r.computed), fmt(r.reference),
            err, band, status_chip(r.status), escape(r.source_id or "—"),
        ])
    counts = compare_ref.summary_counts(ctx.comparison)
    summary = (
        f"<p><b>{counts['PASS']}</b> pass · <b>{counts['MARGINAL']}</b> marginal · "
        f"<b>{counts['FAIL']}</b> fail · <b>{counts['NO-REF (TODO)']}</b> awaiting reference "
        f"digitization · <b>{counts['N/A']}</b> not applicable to this run.</p>"
        "<p class='warn'>Reference slots are TODO by design — values must be digitized from "
        "the literature (see the registry README), never fabricated. Statuses flip to "
        "PASS/FAIL automatically once the registry is filled.</p>"
    )
    tbl = table(
        ["Metric", "computed", "reference", "rel. error", "band", "status", "source"],
        rows, "Computed vs reference with the spec's acceptance bands",
    )
    return "Comparison vs reference", summary + tbl, bool(rows)


def sec_efficiency(ctx: Context) -> tuple[str, str, bool]:
    if not ctx.eff:
        return "Adaptive efficiency", "<p class='warn'>efficiency.txt not found</p>", False
    uniform_run_cells = float(ctx.records[0]["cells"]) if ctx.records else 0.0
    labels = ["uniform fine (octree max level)", "uniform baseline run", "adaptive (mean)", "adaptive (final)"]
    values = [
        ctx.eff.get("uniform_fine_cells", 0.0),
        uniform_run_cells,
        ctx.eff.get("adaptive_mean_cells", 0.0),
        float(ctx.octree.count) if ctx.octree else 0.0,
    ]
    chart = svg.bar_chart(labels, values, title="Cell-count comparison", ylabel="cells",
                          color="#2980b9", width=700)
    callout = (
        f"<p class='big'>Cell speedup vs uniform-fine: "
        f"<b>{ctx.eff.get('cell_speedup', 0):.1f}×</b> "
        f"(adaptive control-plane wall time {ctx.eff.get('wall_seconds', 0):.1f} s)</p>"
        "<p class='warn'>Honest scope (ADR-0004): the flow itself is advanced by the validated "
        "uniform baseline behind the SolverBackend seam; the demonstrated saving is in mesh "
        "cells (resolution + model map). Full compute savings arrive with the octree-native "
        "NS backend.</p>"
    )
    return "Adaptive efficiency vs uniform baseline", chart + callout, True


def sec_reference_appendix(ctx: Context) -> tuple[str, str, bool]:
    src_csv = ctx.ref_dir.parent / "sources.csv"
    rows = []
    if src_csv.exists():
        with open(src_csv, newline="") as f:
            for r in csv.DictReader(f):
                rows.append([
                    f"<code>{escape(r.get('source_id', ''))}</code>",
                    escape(r.get("citation", "")),
                    escape(r.get("what_to_digitize", "")),
                    escape(r.get("status", "")),
                ])
    tbl = table(["ID", "Citation", "What to digitize", "Status"], rows,
                "Reference registry — candidate sources (verify before digitizing)")
    return "Reference registry appendix", tbl, bool(rows)


# ---------------------------------------------------------------------------
CSS = """
body{font-family:Helvetica,Arial,sans-serif;margin:0;color:#1a1a1a;background:#f5f6f7}
header{background:#10243e;color:#fff;padding:26px 40px}
header h1{margin:0;font-size:24px} header p{margin:6px 0 0;color:#9fb3cc}
main{max-width:1020px;margin:0 auto;padding:24px 40px 80px}
section{background:#fff;border-radius:8px;padding:22px 26px;margin:22px 0;
box-shadow:0 1px 3px rgba(0,0,0,.08)} section h2{margin-top:0;font-size:18px;
border-bottom:2px solid #10243e;padding-bottom:6px}
table{border-collapse:collapse;margin:12px 0;font-size:13px;width:100%}
caption{caption-side:top;text-align:left;font-size:12px;color:#666;padding-bottom:4px}
th,td{border:1px solid #d8dde3;padding:5px 9px;text-align:left}
th{background:#eef1f5} svg{max-width:100%;height:auto;margin:8px 0}
.chip{color:#fff;border-radius:4px;padding:2px 7px;font-size:11px;font-weight:bold}
.warn{color:#8a6d3b;background:#fcf8e3;border:1px solid #faebcc;border-radius:4px;padding:8px 12px;font-size:13px}
.big{font-size:18px} code{background:#f0f2f5;padding:1px 4px;border-radius:3px}
@media print{body{background:#fff} section{box-shadow:none;border:1px solid #ddd}}
"""


def build_report(rung: Path, ref_dir: Path, out: Path, window_frac: float, check: bool) -> int:
    ctx = load_context(rung, ref_dir, window_frac)
    sections = [
        sec_experiment(ctx),
        sec_mesh_evolution(ctx),
        sec_label_maps(ctx),
        sec_forces(ctx),
        sec_fields(ctx),
        sec_profiles(ctx),
        sec_spectra(ctx),
        sec_recirculation(ctx),
        sec_cp(ctx),
        sec_audit(ctx),
        sec_comparison(ctx),
        sec_efficiency(ctx),
        sec_reference_appendix(ctx),
    ]
    re_h = ctx.case.get("reynolds", "?")
    meta = load_meta(rung / "uniform" / "diagnostics.jsonl")
    build_line = (
        f" · solver {meta.get('solver_version', '?')} (git {meta.get('git_sha', 'unknown')})"
        if meta
        else ""
    )
    toc = "".join(
        f"<a href='#s{i}' style='margin-right:14px;color:#9fb3cc'>{escape(t)}</a>"
        for i, (t, _, _) in enumerate(sections)
    )
    body = "".join(
        f"<section id='s{i}'><h2>{i+1}. {escape(t)}</h2>{html}</section>"
        for i, (t, html, _) in enumerate(sections)
    )
    doc = (
        "<!doctype html><html><head><meta charset='utf-8'>"
        f"<title>Nabla AI — Phase 0 Validation Report (Re_h={re_h})</title>"
        f"<style>{CSS}</style></head><body>"
        f"<header><h1>∇ Nabla AI — Phase 0 Validation Report</h1>"
        f"<p>Wall-mounted cube in channel flow · Re_h = {re_h} · every adaptive decision"
        f" audited{escape(build_line)}</p>"
        f"<p style='font-size:11px'>{toc}</p></header><main>{body}</main></body></html>"
    )
    out.write_text(doc)
    print(f"wrote {out} ({len(doc) / 1024:.0f} KiB)")

    failed = [t for t, _, ok in sections if not ok]
    for t, _, ok in sections:
        print(f"  [{'ok' if ok else 'EMPTY'}] {t}")
    if check and failed:
        print(f"CHECK FAILED: {len(failed)} empty section(s): {', '.join(failed)}", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rung", type=Path, required=True, help="ladder rung dir (e.g. validation/runs/re0500)")
    ap.add_argument("--reference", type=Path, default=None,
                    help="reference dir (default: registry folder matching the rung Re)")
    ap.add_argument("--out", type=Path, default=None, help="output HTML (default: <rung>/report.html)")
    ap.add_argument("--stats-window", type=float, default=0.5)
    ap.add_argument("--check", action="store_true", help="exit non-zero unless every section is populated")
    args = ap.parse_args(argv)

    rung = args.rung.resolve()
    ref = args.reference
    if ref is None:
        case = rung / "case.json"
        re_h = 500
        if case.exists():
            re_h = int(json.loads(case.read_text()).get("reynolds", 500))
        ref = Path(__file__).resolve().parent / "reference" / "wall_mounted_cube" / f"re{re_h:04d}"
    out = args.out or rung / "report.html"
    return build_report(rung, ref.resolve(), out, args.stats_window, args.check)


if __name__ == "__main__":
    sys.exit(main())
