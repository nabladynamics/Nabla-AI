"""Run the canonical validation cases and emit a Markdown report.

For every ``cases/<name>.spec`` paired with ``reference/<name>.expected`` this
invokes the compiled solver (file-based IPC), compares the result, and writes a
``report.md`` summary. Pure standard library.

Usage:
    python generate_report.py [--solver PATH] [--out report.md] [--strict]
"""

from __future__ import annotations

import argparse
import datetime as dt
import subprocess
import sys
import tempfile
from pathlib import Path

from compare import compare, load_kv

ROOT = Path(__file__).resolve().parent
DEFAULT_SOLVER = ROOT.parent / "core" / "build" / "nabla_solve"


def discover_cases() -> list[tuple[str, Path, Path]]:
    cases: list[tuple[str, Path, Path]] = []
    for spec in sorted((ROOT / "cases").glob("*.spec")):
        expected = ROOT / "reference" / f"{spec.stem}.expected"
        if expected.exists():
            cases.append((spec.stem, spec, expected))
    return cases


def run_solver(solver: Path, spec: Path) -> dict[str, str]:
    with tempfile.NamedTemporaryFile(suffix=".result", delete=False) as handle:
        out_path = Path(handle.name)
    subprocess.run(
        [str(solver), "--input", str(spec), "--output", str(out_path)],
        check=True,
    )
    return load_kv(out_path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--solver", type=Path, default=DEFAULT_SOLVER)
    parser.add_argument("--out", type=Path, default=ROOT / "report.md")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit non-zero if the solver binary is missing",
    )
    args = parser.parse_args(argv)

    timestamp = dt.datetime.now(dt.UTC).isoformat(timespec="seconds")
    lines = ["# Nabla AI — Validation Report", "", f"_Generated {timestamp}_", ""]

    if not args.solver.exists():
        message = (
            f"Solver binary not found at `{args.solver}`. "
            "Build the core first (see scripts/dev.sh)."
        )
        lines += [f"> **Skipped:** {message}", ""]
        args.out.write_text("\n".join(lines))
        print(message)
        return 1 if args.strict else 0

    cases = discover_cases()
    total_failed = 0
    for name, spec, expected in cases:
        result = run_solver(args.solver, spec)
        checks = compare(result, load_kv(expected))
        failed = sum(1 for c in checks if not c.ok)
        total_failed += failed
        status = "✅ PASS" if failed == 0 else "❌ FAIL"
        lines += [
            f"## {name} — {status}",
            "",
            "| check | result | status |",
            "| ----- | ------ | ------ |",
        ]
        lines += [f"| {c.name} | {c.detail} | {'✅' if c.ok else '❌'} |" for c in checks]
        lines.append("")

    args.out.write_text("\n".join(lines))
    print(f"Wrote {args.out} — {len(cases)} case(s), {total_failed} failed check(s)")
    return 1 if total_failed else 0


if __name__ == "__main__":
    sys.exit(main())
