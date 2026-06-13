"""Phase-0 experiment runner: the wall-mounted-cube Reynolds ladder.

    python -m validation.run_ladder [--only-re 500] [--steps 240] ...

Executes Re_h = 500 -> 1000 -> 1700 -> 3000 sequentially. Each rung runs the
uniform NS case (diagnostics + snapshots + checkpoints) and then the adaptive
stage (audit trail + efficiency). Interrupted rungs resume from the latest
solver checkpoint; completed rungs are skipped, so re-running the command is
always safe.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re as re_mod
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parent.parent
DEFAULT_LADDER = (500, 1000, 1700, 3000)
_CKPT_RE = re_mod.compile(r"checkpoint_(\d+)\.ckpt$")


def find_solver(explicit: Path | None) -> Path:
    if explicit:
        if not explicit.exists():
            raise SystemExit(f"solver not found: {explicit}")
        return explicit.resolve()
    for cand in (REPO / "core/build/nabla_solve", REPO / "core/build-test/nabla_solve"):
        if cand.exists():
            return cand.resolve()
    raise SystemExit("nabla_solve not found — build the core first (scripts/build-core.sh)")


def solver_version(solver: Path) -> str:
    out = subprocess.run([str(solver), "--version"], capture_output=True, text=True)
    return out.stdout.strip() or "unknown"


def load_state(rung: Path) -> dict[str, Any]:
    p = rung / "rung_state.json"
    if p.exists():
        return json.loads(p.read_text())
    return {"stages": {}}


def save_state(rung: Path, state: dict[str, Any]) -> None:
    state["updated"] = dt.datetime.now().isoformat(timespec="seconds")
    (rung / "rung_state.json").write_text(json.dumps(state, indent=2) + "\n")


def steps_done(uniform_dir: Path) -> int:
    diag = uniform_dir / "diagnostics.jsonl"
    if not diag.exists():
        return 0
    last = 0
    for line in diag.read_text().splitlines():
        line = line.strip()
        if line:
            try:
                last = max(last, int(json.loads(line)["step"]))
            except (ValueError, KeyError):
                continue
    return last


def latest_checkpoint(uniform_dir: Path) -> tuple[int, Path] | None:
    best: tuple[int, Path] | None = None
    for p in uniform_dir.glob("checkpoint_*.ckpt"):
        m = _CKPT_RE.search(p.name)
        if m:
            n = int(m.group(1))
            if best is None or n > best[0]:
                best = (n, p)
    return best


def run_uniform(args: argparse.Namespace, solver: Path, rung: Path, re_h: int) -> float:
    """Run (or resume) the uniform NS stage; returns wall seconds spent now."""
    uniform = rung / "uniform"
    uniform.mkdir(parents=True, exist_ok=True)
    done = steps_done(uniform)
    if done >= args.steps:
        print(f"  uniform: {done}/{args.steps} steps — already complete, skipping")
        return 0.0

    case: dict[str, Any] = {
        "type": "wall-mounted-cube",
        "name": f"wall-mounted-cube-re{re_h}",
        "reynolds": re_h,
        "resolution": args.res,
        "cfl": 0.7,
        "convection": "weno5",
        "max_steps": args.steps,
        "snapshot_every": args.snapshot_every,
        "checkpoint_every": args.checkpoint_every,
        "run_dir": str(uniform),
    }
    ckpt = latest_checkpoint(uniform) if done > 0 else None
    if ckpt:
        case["restart_from"] = str(ckpt[1])
        case["max_steps"] = args.steps - ckpt[0]
        print(f"  uniform: resuming from {ckpt[1].name} (step {ckpt[0]}; {case['max_steps']} steps remain)")
    else:
        print(f"  uniform: starting fresh ({args.steps} steps)")
    # ensure_ascii=False: keep paths as raw UTF-8 (no \uXXXX escapes).
    (rung / "case.json").write_text(
        json.dumps({**case, "max_steps": args.steps, "run_dir": str(uniform)},
                   indent=2, ensure_ascii=False) + "\n")
    case_file = rung / ("case_resume.json" if ckpt else "case_initial.json")
    case_file.write_text(json.dumps(case, indent=2, ensure_ascii=False) + "\n")

    t0 = time.monotonic()
    subprocess.run([str(solver), "run", str(case_file)], check=True)
    return time.monotonic() - t0


def run_adaptive(args: argparse.Namespace, solver: Path, rung: Path, re_h: int) -> float:
    adaptive = rung / "adaptive"
    if (adaptive / "audit_summary.md").exists() and (adaptive / "adaptive_final.vtu").exists():
        print("  adaptive: outputs present — skipping")
        return 0.0
    print(f"  adaptive: running ({args.adapt_warmup} warmup + {args.adapt_steps} adaptive steps)")
    t0 = time.monotonic()
    subprocess.run(
        [
            str(solver), "adapt",
            "--res", str(args.res), "--re", str(re_h),
            "--base-level", str(args.adapt_base_level),
            "--max-level", str(args.adapt_max_level),
            "--warmup", str(args.adapt_warmup),
            "--steps", str(args.adapt_steps),
            "--run-dir", str(adaptive),
        ],
        check=True,
    )
    return time.monotonic() - t0


def run_rung(args: argparse.Namespace, solver: Path, re_h: int) -> None:
    rung = args.out / f"re{re_h:04d}"
    rung.mkdir(parents=True, exist_ok=True)
    print(f"== rung Re_h = {re_h}  ({rung}) ==")
    state = load_state(rung)
    state.setdefault("re", re_h)
    state["resolution"] = args.res
    state["steps_target"] = args.steps
    state["solver"] = solver_version(solver)

    ust = state["stages"].setdefault("uniform", {"wall_s": 0.0, "resumes": 0})
    resumed = steps_done(rung / "uniform") > 0
    wall = run_uniform(args, solver, rung, re_h)
    if wall > 0.0 and resumed:
        ust["resumes"] = int(ust.get("resumes", 0)) + 1
    ust["wall_s"] = float(ust.get("wall_s", 0.0)) + wall
    ust["steps_done"] = steps_done(rung / "uniform")
    ust["status"] = "done" if ust["steps_done"] >= args.steps else "incomplete"
    save_state(rung, state)

    ast = state["stages"].setdefault("adaptive", {"wall_s": 0.0})
    ast["wall_s"] = float(ast.get("wall_s", 0.0)) + run_adaptive(args, solver, rung, re_h)
    ast["status"] = "done" if (rung / "adaptive" / "audit_summary.md").exists() else "incomplete"
    save_state(rung, state)
    print(f"  rung state: uniform={ust['status']} ({ust['steps_done']} steps), adaptive={ast['status']}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--solver", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=REPO / "validation" / "runs")
    ap.add_argument("--re", type=int, nargs="*", default=list(DEFAULT_LADDER))
    ap.add_argument("--only-re", type=int, default=None)
    ap.add_argument("--res", type=int, default=6, help="cells per cube height h")
    ap.add_argument("--steps", type=int, default=240)
    ap.add_argument("--snapshot-every", type=int, default=20)
    ap.add_argument("--checkpoint-every", type=int, default=40)
    ap.add_argument("--adapt-warmup", type=int, default=25)
    ap.add_argument("--adapt-steps", type=int, default=18)
    ap.add_argument("--adapt-base-level", type=int, default=3)
    ap.add_argument("--adapt-max-level", type=int, default=6)
    args = ap.parse_args(argv)
    args.out = args.out.resolve()

    solver = find_solver(args.solver)
    ladder = [args.only_re] if args.only_re else args.re
    print(f"Nabla AI Re ladder: {ladder}  (solver: {solver_version(solver)})")
    for re_h in ladder:
        run_rung(args, solver, re_h)
    print("ladder complete — generate reports with: python -m validation.make_report --rung <rung dir>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
