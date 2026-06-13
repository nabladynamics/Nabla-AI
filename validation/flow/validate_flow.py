"""Automated flow validations for the baseline NS solver (stdlib only).

Runs the solver on the lid-driven cavity (Re=100) and the Poiseuille channel,
extracts centerline / outlet profiles from the produced .vtu, and checks the L2
error against the reference (Ghia et al.) / analytic solution. Pass threshold:
2% relative L2.

    python validate_flow.py --solver ../../core/build/nabla_solve

Exits non-zero if any case fails.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
THRESHOLD = 0.02  # 2% relative L2


def load_ghia(path: Path) -> tuple[list[float], list[float]]:
    ys, us = [], []
    for line in path.read_text().splitlines():
        s = line.split("#", 1)[0].strip()
        if not s:
            continue
        y, u = s.split(",")
        ys.append(float(y))
        us.append(float(u))
    return ys, us


def parse_vtu(path: Path):
    """Return (centers, fields) where centers[c]=(x,y,z), fields['u'][c]=value."""
    root = ET.parse(path).getroot()
    pts = list(map(float, root.find(".//Points/DataArray").text.split()))
    arrays = {da.get("Name"): list(map(float, da.text.split()))
              for da in root.find(".//CellData").iter("DataArray")}
    ncell = len(arrays["u"])
    centers = []
    for c in range(ncell):
        xs = ys = zs = 0.0
        for k in range(8):
            b = (c * 8 + k) * 3
            xs += pts[b]
            ys += pts[b + 1]
            zs += pts[b + 2]
        centers.append((xs / 8, ys / 8, zs / 8))
    return centers, arrays


def column_nearest_x(centers, field, target_x):
    xs = sorted({round(c[0], 9) for c in centers})
    xcol = min(xs, key=lambda x: abs(x - target_x))
    pts = [(centers[c][1], field[c]) for c in range(len(field))
           if abs(centers[c][0] - xcol) < 1e-9]
    pts.sort()
    return [p[0] for p in pts], [p[1] for p in pts]


def interp(xs, ys, x):
    for i in range(1, len(xs)):
        if x <= xs[i]:
            t = (x - xs[i - 1]) / (xs[i] - xs[i - 1])
            return ys[i - 1] + t * (ys[i] - ys[i - 1])
    return ys[-1]


def rel_l2(num_pairs) -> float:
    num = sum((a - b) ** 2 for a, b in num_pairs)
    den = sum(b * b for _, b in num_pairs)
    return math.sqrt(num / den) if den > 0 else math.sqrt(num)


def run_case(solver: Path, case_json: Path, workdir: Path) -> Path:
    spec = json.loads(case_json.read_text())
    run_dir = spec.get("run_dir", "run")
    subprocess.run([str(solver), "run", str(case_json)], cwd=workdir, check=True)
    return workdir / run_dir / "final.vtu"


def validate_cavity(solver: Path, workdir: Path) -> tuple[bool, float]:
    vtu = run_case(solver, HERE / "cavity_re100.json", workdir)
    centers, fields = parse_vtu(vtu)
    ys, us = column_nearest_x(centers, fields["u"], 0.5)
    gy, gu = load_ghia(HERE / "ghia_re100_u.csv")
    pairs = [(interp(ys, us, gy[k]), gu[k]) for k in range(1, len(gy) - 1)]  # interior
    err = rel_l2(pairs)
    return err <= THRESHOLD, err


def validate_poiseuille(solver: Path, workdir: Path) -> tuple[bool, float]:
    vtu = run_case(solver, HERE / "channel_poiseuille.json", workdir)
    centers, fields = parse_vtu(vtu)
    max_x = max(c[0] for c in centers)
    ys, us = column_nearest_x(centers, fields["u"], max_x)
    # analytic parabola: 1.5*Ubulk*(1 - (2(y-0.5))^2), Ubulk=1, Ly=1
    pairs = [(us[i], 1.5 * (1.0 - (2 * (ys[i] - 0.5)) ** 2)) for i in range(len(ys))]
    err = rel_l2(pairs)
    return err <= THRESHOLD, err


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--solver", type=Path,
                    default=HERE.parent.parent / "core" / "build" / "nabla_solve")
    ap.add_argument("--only", choices=["cavity", "poiseuille"], default=None)
    args = ap.parse_args(argv)
    args.solver = args.solver.resolve()  # subprocesses run in a temp cwd

    if not args.solver.exists():
        print(f"solver binary not found: {args.solver}", file=sys.stderr)
        return 1

    cases = []
    if args.only in (None, "poiseuille"):
        cases.append(("Poiseuille (analytic)", validate_poiseuille))
    if args.only in (None, "cavity"):
        cases.append(("Lid cavity Re=100 (Ghia)", validate_cavity))

    all_ok = True
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for label, fn in cases:
            ok, err = fn(args.solver, work)
            all_ok = all_ok and ok
            print(f"[{'PASS' if ok else 'FAIL'}] {label}: L2 = {100 * err:.3f}%  "
                  f"(threshold {100 * THRESHOLD:.0f}%)")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
