"""File-based IPC with the solver core.

The backend never runs physics itself. It writes a spec file, invokes
``nabla_solve --input <spec> --output <result>``, then parses the result file.
This module owns the (de)serialization half of that contract.
"""

from __future__ import annotations

from app.models import SolveResult


def parse_result(text: str) -> SolveResult:
    """Parse the core's ``key = value`` result file into a :class:`SolveResult`.

    Lines may carry ``#`` comments; blank/comment lines are skipped.
    """
    values: dict[str, str] = {}
    for raw_line in text.splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = value.strip()

    return SolveResult(
        steps_run=int(values["steps_run"]),
        final_max=float(values["final_max"]),
        final_mean=float(values["final_mean"]),
        stability=float(values["stability"]),
        stable=values["stable"].lower() == "true",
    )
