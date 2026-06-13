"""Stand-in for the nabla_solve binary used by the lifecycle tests.

Implements just enough of the real CLI contract:

  stub_solver.py --version
  stub_solver.py ingest <stl> --case <name> --out-vtu <path> --report <path>
  stub_solver.py run <case.json>            (honors restart_from + max_steps)

``run`` appends one diagnostics + one audit JSON line per step, writes
checkpoints every ``checkpoint_every`` steps (each checkpoint records its step
number, like the real binary's restart semantics), and sleeps
``NABLA_STUB_STEP_SLEEP`` seconds per step so tests can pause it mid-flight.
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

# Mirrors the real solver's build-provenance line (first line of every fresh
# diagnostics.jsonl / audit.jsonl) so tests exercise the meta-tolerant parsers.
META_LINE = (
    json.dumps(
        {
            "event": "meta",
            "solver": "stub_solver",
            "solver_version": "0.0.0-stub",
            "git_sha": "stub000000",
        }
    )
    + "\n"
)


def _ingest(argv: list[str]) -> int:
    stl = Path(argv[0])
    opts = dict(zip(argv[1::2], argv[2::2], strict=False))
    if not stl.is_file():
        print(f"error: cannot open {stl}", file=sys.stderr)
        return 1
    report = {
        "case": opts.get("--case", "wall-mounted-cube"),
        "schema": "nabla.geometry_report/1",
        "cleaning": {"watertight": True, "triangles": 12, "vertices": 8},
        "bounding_box": {"min": [0, 0, 0], "max": [1, 1, 1], "size": [1, 1, 1]},
        "characteristic_length": 1.732,
        "features": {"sharp_edges": {"count": 12}, "corners": {"count": 8}},
        "mesh": {"cells": 512, "balanced": True},
    }
    Path(opts["--report"]).write_text(json.dumps(report))
    Path(opts["--out-vtu"]).write_text("<VTKFile><!-- stub mesh --></VTKFile>\n")
    print("stub ingest ok")
    return 0


def _run(argv: list[str]) -> int:
    case = json.loads(Path(argv[0]).read_text())
    run_dir = Path(case["run_dir"])
    run_dir.mkdir(parents=True, exist_ok=True)
    max_steps = int(case["max_steps"])
    ckpt_every = int(case.get("checkpoint_every", 5))
    snap_every = int(case.get("snapshot_every", 0))
    sleep_s = float(os.environ.get("NABLA_STUB_STEP_SLEEP", "0.01"))

    start = 0
    restart = case.get("restart_from")
    if restart:
        start = int(json.loads(Path(restart).read_text())["step"])

    mode = "a" if restart else "w"
    with (
        open(run_dir / "diagnostics.jsonl", mode) as diag,
        open(run_dir / "audit.jsonl", mode) as audit,
    ):
        if not restart:
            diag.write(META_LINE)
            audit.write(META_LINE)
        for step in range(start + 1, start + max_steps + 1):
            t = step * 0.05
            diag.write(
                json.dumps(
                    {
                        "step": step,
                        "t": t,
                        "dt": 0.05,
                        "cd": 0.9 + 0.01 * (step % 3),
                        "cl": 0.3,
                        "momentum_residual": 1.0 / step,
                        "continuity_residual": 1e-7,
                        "mass_error": 1e-10,
                        "cells": 512,
                        "accepted": True,
                    }
                )
                + "\n"
            )
            diag.flush()
            audit.write(json.dumps({"event": "metrics", "step": step, "cells": 512}) + "\n")
            audit.flush()
            if ckpt_every and step % ckpt_every == 0:
                (run_dir / f"checkpoint_{step}.ckpt").write_text(json.dumps({"step": step}))
            if snap_every and step % snap_every == 0:
                _write_uniform_vtu(run_dir / f"snapshot_{step}.vtu")
            time.sleep(sleep_s)

    _write_uniform_vtu(run_dir / "final.vtu")
    (run_dir / "final.ckpt").write_text(json.dumps({"step": start + max_steps}))
    return 0


def _write_uniform_vtu(path: Path) -> None:
    """Small but REAL uniform-grid snapshot (10x4x4 cells, h=4 cells) with the
    baseline writer's cell-data layout: u,v,w,p,speed,solid. Carries a solid
    block at x in [1,2] and a reverse-flow pocket behind it so the analysis
    endpoints (recirculation, cores) have structure to find."""
    nx, ny, nz, d = 10, 4, 4, 0.25
    points: list[str] = []
    u: list[float] = []
    v: list[float] = []
    w: list[float] = []
    p: list[float] = []
    solid: list[float] = []
    for k in range(nz):
        for j in range(ny):
            for i in range(nx):
                x0, y0, z0 = i * d, j * d, k * d
                x1, y1, z1 = x0 + d, y0 + d, z0 + d
                points.append(
                    f"{x0} {y0} {z0} {x1} {y0} {z0} {x1} {y1} {z0} {x0} {y1} {z0} "
                    f"{x0} {y0} {z1} {x1} {y0} {z1} {x1} {y1} {z1} {x0} {y1} {z1}"
                )
                cx, cy = x0 + d / 2, y0 + d / 2
                inside = 1.0 <= cx <= 2.0 and cy <= 1.0 and 0.5 <= z0 + d / 2 <= 1.5
                solid.append(1.0 if inside else 0.0)
                bubble = 2.0 <= cx <= 2.4 and cy <= 0.3
                # pure solid-body-rotation patch upstream (i 0..2, j 1..3): the
                # strain tensor vanishes identically there, so the Q-criterion
                # is genuinely positive (a shear bubble alone has Q <= 0).
                vortex = i <= 2 and j >= 1
                if inside:
                    u.append(0.0)
                    v.append(0.0)
                elif vortex:
                    u.append(-2.0 * (cy - 0.625))
                    v.append(2.0 * (cx - 0.375))
                else:
                    u.append(-0.2 if bubble else 1.0)
                    v.append(0.05 * cy)
                w.append(0.0)
                p.append(0.5 - 0.1 * cx)
    n = nx * ny * nz

    def arr(name: str, values: list[float], kind: str = "Float64") -> str:
        body = " ".join(str(value) for value in values)
        return f'<DataArray type="{kind}" Name="{name}" format="ascii">{body}</DataArray>'

    points_str = " ".join(points)
    types_str = " ".join("12" for _ in range(n))
    path.write_text(
        f"""<?xml version="1.0"?>
<VTKFile type="UnstructuredGrid" version="1.0" byte_order="LittleEndian" header_type="UInt64">
  <UnstructuredGrid><Piece NumberOfPoints="{8 * n}" NumberOfCells="{n}">
    <Points>
      <DataArray type="Float64" NumberOfComponents="3" format="ascii">{points_str}</DataArray>
    </Points>
    <Cells>
      {arr("connectivity", list(range(8 * n)), "Int64")}
      {arr("offsets", [8 * (i + 1) for i in range(n)], "Int64")}
      <DataArray type="UInt8" Name="types" format="ascii">{types_str}</DataArray>
    </Cells>
    <CellData Scalars="u">
      {arr("u", u)}
      {arr("v", v)}
      {arr("w", w)}
      {arr("p", p)}
      {arr("solid", solid)}
    </CellData>
  </Piece></UnstructuredGrid></VTKFile>
"""
    )


def _cell_block(x: float, y: float, z: float, s: float) -> str:
    x1, y1, z1 = x + s, y + s, z + s
    return (
        f"{x} {y} {z} {x1} {y} {z} {x1} {y1} {z} {x} {y1} {z} "
        f"{x} {y} {z1} {x1} {y} {z1} {x1} {y1} {z1} {x} {y1} {z1}"
    )


def _write_octree_vtu(path: Path, step: int) -> None:
    """Tiny octree-style snapshot in the real writer's layout: 4 cells with
    level / mask / physics_mode cell data. Cell levels grow with `step` so a
    poller can observe 'refinement'."""
    cells = [
        (0.0, 0.0, 0.0, 1.0, 2, 0, 3),  # x,y,z,size,level,mask,mode
        (1.0, 0.0, 0.0, 1.0, min(2 + step, 6), 0, 0),
        (2.0, 0.0, 0.0, 0.5, min(3 + step, 6), 1, 4),
        (2.0, 0.5, 0.0, 0.5, 3, 2, 0),  # solid
    ]
    points = " ".join(_cell_block(x, y, z, s) for x, y, z, s, _, _, _ in cells)
    n = len(cells)
    conn = " ".join(str(i) for i in range(8 * n))
    offsets = " ".join(str(8 * (i + 1)) for i in range(n))
    types = " ".join("12" for _ in range(n))
    levels = " ".join(str(c[4]) for c in cells)
    masks = " ".join(str(c[5]) for c in cells)
    modes = " ".join(str(c[6]) for c in cells)
    path.write_text(
        f"""<?xml version="1.0"?>
<VTKFile type="UnstructuredGrid" version="1.0" byte_order="LittleEndian" header_type="UInt64">
  <UnstructuredGrid><Piece NumberOfPoints="{8 * n}" NumberOfCells="{n}">
    <Points>
      <DataArray type="Float64" NumberOfComponents="3" format="ascii">{points}</DataArray>
    </Points>
    <Cells>
      <DataArray type="Int64" Name="connectivity" format="ascii">{conn}</DataArray>
      <DataArray type="Int64" Name="offsets" format="ascii">{offsets}</DataArray>
      <DataArray type="UInt8" Name="types" format="ascii">{types}</DataArray>
    </Cells>
    <CellData Scalars="level">
      <DataArray type="Int32" Name="level" format="ascii">{levels}</DataArray>
      <DataArray type="UInt8" Name="mask" format="ascii">{masks}</DataArray>
      <DataArray type="UInt8" Name="physics_mode" format="ascii">{modes}</DataArray>
    </CellData>
  </Piece></UnstructuredGrid></VTKFile>
"""
    )


def _adapt(argv: list[str]) -> int:
    opts = dict(zip(argv[0::2], argv[1::2], strict=False))
    run_dir = Path(opts["--run-dir"])
    run_dir.mkdir(parents=True, exist_ok=True)
    warmup = int(opts.get("--warmup", "2"))
    steps = int(opts.get("--steps", "5"))
    sleep_s = float(os.environ.get("NABLA_STUB_STEP_SLEEP", "0.01"))

    _write_octree_vtu(run_dir / "adaptive_latest.vtu", 0)
    with (
        open(run_dir / "diagnostics.jsonl", "w") as diag,
        open(run_dir / "audit.jsonl", "w") as audit,
    ):
        diag.write(META_LINE)
        audit.write(META_LINE)
        global_step = 0
        for _ in range(warmup):
            global_step += 1
            diag.write(
                json.dumps(
                    {
                        "step": global_step,
                        "t": global_step * 0.06,
                        "dt": 0.06,
                        "cfl": 0.7,
                        "momentum_residual": 1.0 / global_step,
                        "continuity_residual": 1e-7,
                        "mass_error": 1e-10,
                        "cd": 0.9,
                        "cl": 0.3,
                        "poisson_iters": 1000,
                        "cells": 512,
                        "accepted": True,
                    }
                )
                + "\n"
            )
            diag.flush()
            time.sleep(sleep_s)
        for s in range(1, steps + 1):
            global_step += 1
            cells = 512 + 700 * s
            for line in (
                {"event": "step_begin", "step": s, "t": 0.0, "dt": 0.0},
                {
                    "event": "refine",
                    "step": s,
                    "count": 100,
                    "reason": "sensor thresholds exceeded",
                },
                {
                    "event": "model_change",
                    "step": s,
                    "cell": 4096 + s,
                    "from": "NEAR_WALL",
                    "to": "FULL_NS",
                    "reason": "reverse flow at wall: log-law assumption violated",
                },
                {
                    "event": "acceptance",
                    "step": s,
                    "region": "NEAR_WALL",
                    "mode": "NEAR_WALL",
                    "accepted": True,
                    "reason": "1300 accepted, 2 rejected -> FULL_NS",
                },
                {
                    "event": "acceptance",
                    "step": s,
                    "region": "INVISCID",
                    "mode": "INVISCID",
                    "accepted": True,
                    "reason": "7000 accepted, 0 rejected -> FULL_NS",
                },
                {
                    "event": "acceptance",
                    "step": s,
                    "region": "hard-guard-zones",
                    "mode": "FULL_NS",
                    "accepted": True,
                    "reason": "987 reduced proposals overridden to FULL_NS",
                },
                {"event": "step_decision", "step": s, "decision": "accept", "detail": "ok"},
                {"event": "metrics", "step": s, "cells": cells, "cd": 0.9, "cl": 0.31},
                {"event": "step_end", "step": s},
            ):
                audit.write(json.dumps(line) + "\n")
            audit.flush()
            diag.write(
                json.dumps(
                    {
                        "step": global_step,
                        "t": global_step * 0.06,
                        "dt": 0.06,
                        "cfl": 0.7,
                        "momentum_residual": 0.05 / s,
                        "continuity_residual": 1e-7,
                        "mass_error": 1e-10,
                        "cd": 0.9,
                        "cl": 0.31,
                        "poisson_iters": 1000,
                        "cells": cells,
                        "accepted": True,
                    }
                )
                + "\n"
            )
            diag.flush()
            tmp = run_dir / "adaptive_latest.vtu.tmp"
            _write_octree_vtu(tmp, s)
            tmp.rename(run_dir / "adaptive_latest.vtu")
            time.sleep(sleep_s)

    (run_dir / "audit_summary.md").write_text("# stub audit summary\n")
    (run_dir / "efficiency.txt").write_text(
        "uniform-fine cells = 262144, adaptive mean cells = 4000.0, "
        "cell speedup = 65.5x, wall = 1.0 s\n"
    )
    _write_octree_vtu(run_dir / "adaptive_final.vtu", steps)
    return 0


def main() -> int:
    argv = sys.argv[1:]
    if not argv:
        return 2
    if argv[0] in ("-v", "--version"):
        print("nabla_solve 0.1.0-stub")
        return 0
    if argv[0] == "ingest":
        return _ingest(argv[1:])
    if argv[0] == "run":
        return _run(argv[1:])
    if argv[0] == "adapt":
        return _adapt(argv[1:])
    print(f"stub: unknown command {argv[0]}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
