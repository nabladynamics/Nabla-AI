"""Compare a ``nabla_solve`` result file against reference data.

Pure standard library — no third-party dependencies — so it runs anywhere a
Python 3.12 interpreter exists. The reference file is ``key = value`` like the
solver result; numeric keys may carry a companion ``<key>_tol`` absolute
tolerance, otherwise the comparison is exact string equality.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path


def load_kv(path: Path) -> dict[str, str]:
    """Parse a ``key = value`` file (``#`` comments and blanks ignored)."""
    values: dict[str, str] = {}
    for raw_line in path.read_text().splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip()
    return values


@dataclass(frozen=True)
class Check:
    name: str
    ok: bool
    detail: str


def compare(result: dict[str, str], expected: dict[str, str]) -> list[Check]:
    """Return a Check per expected key (``*_tol`` helper keys are skipped)."""
    checks: list[Check] = []
    for key, expected_raw in expected.items():
        if key.endswith("_tol"):
            continue
        if key not in result:
            checks.append(Check(key, ok=False, detail="missing from result"))
            continue

        tol_raw = expected.get(f"{key}_tol")
        actual_raw = result[key]
        if tol_raw is not None:
            actual = float(actual_raw)
            expected_value = float(expected_raw)
            error = abs(actual - expected_value)
            ok = error <= float(tol_raw)
            detail = f"|{actual} - {expected_value}| = {error:.3e} (tol {tol_raw})"
        else:
            ok = actual_raw == expected_raw
            detail = f"{actual_raw!r} {'==' if ok else '!='} {expected_raw!r}"
        checks.append(Check(key, ok=ok, detail=detail))
    return checks


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", required=True, type=Path, help="solver result file")
    parser.add_argument("--expected", required=True, type=Path, help="reference file")
    args = parser.parse_args(argv)

    checks = compare(load_kv(args.result), load_kv(args.expected))
    failed = [c for c in checks if not c.ok]
    for check in checks:
        print(f"[{'PASS' if check.ok else 'FAIL'}] {check.name}: {check.detail}")
    print(f"\n{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
