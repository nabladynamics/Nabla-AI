"""Post-simulation endpoints: field bundles, analysis, exports, report."""

from __future__ import annotations

import csv
import io
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Annotated, cast

from fastapi import APIRouter, Depends, HTTPException, Query, Request, Response
from fastapi.responses import FileResponse
from pydantic import BaseModel
from starlette.background import BackgroundTask

from app.analysis import AnalysisResponse, load_diagnostics, run_analysis
from app.fields import FieldBundle, field_bundle
from app.models import CaseConfig
from app.services import RunService

router = APIRouter(prefix="/runs", tags=["post"])

_SNAP_RE = re.compile(r"snapshot_(\d+)\.vtu$")


def get_service(request: Request) -> RunService:
    return cast(RunService, request.app.state.service)


ServiceDep = Annotated[RunService, Depends(get_service)]


def _workspace(service: RunService, run_id: str) -> Path:
    if service.store.get(run_id) is None:
        raise HTTPException(status_code=404, detail=f"run not found: {run_id}")
    return service.artifacts.workspace(run_id)


class SnapshotList(BaseModel):
    steps: list[int]
    has_final: bool


@router.get("/{run_id}/snapshots", response_model=SnapshotList)
def list_snapshots(service: ServiceDep, run_id: str) -> SnapshotList:
    ws = _workspace(service, run_id)
    steps = sorted(
        int(m.group(1)) for p in ws.glob("snapshot_*.vtu") if (m := _SNAP_RE.search(p.name))
    )
    return SnapshotList(steps=steps, has_final=(ws / "final.vtu").is_file())


@router.get("/{run_id}/field", response_model=FieldBundle)
def get_field(
    service: ServiceDep,
    run_id: str,
    step: int = Query(default=-1, description="-1 = final field"),
    stride: int = Query(default=0, ge=0, le=16, description="0 = auto-decimate"),
) -> FieldBundle:
    """Decimated field bundle (base64 Float32) from a snapshot — the browser
    never parses raw .vtu or receives more cells than a laptop GPU enjoys."""
    ws = _workspace(service, run_id)
    path = ws / ("final.vtu" if step < 0 else f"snapshot_{step}.vtu")
    if not path.is_file():
        raise HTTPException(status_code=404, detail=f"snapshot not found: {path.name}")
    try:
        return field_bundle(path, step=step, stride=stride)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc


@router.get("/{run_id}/analysis", response_model=AnalysisResponse)
def get_analysis(
    service: ServiceDep,
    run_id: str,
    window: float = Query(default=0.5, gt=0.0, le=1.0),
) -> AnalysisResponse:
    ws = _workspace(service, run_id)
    run = service.store.get(run_id)
    assert run is not None
    config = CaseConfig.model_validate({**CaseConfig().model_dump(), **run.config})
    registry = (
        service.settings.repo_root / "validation" / "reference" / "wall_mounted_cube"
    ).resolve()
    return run_analysis(ws, reynolds=config.reynolds, registry=registry, window_frac=window)


@router.get("/{run_id}/forces.csv")
def forces_csv(service: ServiceDep, run_id: str) -> Response:
    ws = _workspace(service, run_id)
    diag = ws / "diagnostics.jsonl"
    if not diag.is_file():
        raise HTTPException(status_code=404, detail="no diagnostics yet")
    buffer = io.StringIO()
    writer = csv.writer(buffer)
    writer.writerow(
        ["step", "t", "dt", "cd", "cl", "fd", "fl",
         "momentum_residual", "continuity_residual", "cells"]
    )
    for record in load_diagnostics(diag):
        cd, cl = float(record["cd"]), float(record["cl"])
        writer.writerow(
            [
                record["step"], record["t"], record["dt"], cd, cl,
                0.5 * cd, 0.5 * cl,  # F = 0.5*rho*U^2*A*C with solver units rho=U=A=1
                record["momentum_residual"], record["continuity_residual"], record["cells"],
            ]
        )
    return Response(
        content=buffer.getvalue(),
        media_type="text/csv",
        headers={"Content-Disposition": f'attachment; filename="forces_{run_id}.csv"'},
    )


@router.get("/{run_id}/bundle.zip")
def bundle_zip(
    service: ServiceDep,
    run_id: str,
    suffix: str = Query(default=".vtu", pattern=r"^[A-Za-z0-9.]+$"),
) -> FileResponse:
    """ZIP of all artifacts matching the suffix (default: the VTU bundle)."""
    ws = _workspace(service, run_id)
    files = sorted(p for p in ws.rglob(f"*{suffix}") if p.is_file())
    if not files:
        raise HTTPException(status_code=404, detail=f"no artifacts matching *{suffix}")
    # delete=False is required: the file outlives this scope and is removed by
    # the FileResponse background task after the download completes.
    tmp = tempfile.NamedTemporaryFile(suffix=".zip", delete=False)  # noqa: SIM115
    with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_STORED) as archive:
        for path in files:
            archive.write(path, arcname=str(path.relative_to(ws)))
    tmp.close()
    return FileResponse(
        tmp.name,
        media_type="application/zip",
        filename=f"{run_id}_{suffix.lstrip('.')}_bundle.zip",
        background=BackgroundTask(lambda: Path(tmp.name).unlink(missing_ok=True)),
    )


class ReportResponse(BaseModel):
    artifact: str


@router.post("/{run_id}/report", response_model=ReportResponse)
def generate_report(service: ServiceDep, run_id: str) -> ReportResponse:
    """Generate the Phase-0 validation report for this run by invoking the
    validation harness (`python -m validation.make_report`) on a rung-shaped
    view of the workspace. Serve/download it via the artifacts endpoint and
    print to PDF from the browser."""
    ws = _workspace(service, run_id)
    repo_root = service.settings.repo_root.resolve()
    if not (repo_root / "validation" / "make_report.py").is_file():
        raise HTTPException(
            status_code=503, detail="validation harness not found (NABLA_REPO_ROOT)"
        )

    with tempfile.TemporaryDirectory() as tmp:
        rung = Path(tmp) / "rung"
        rung.mkdir()
        # the harness expects a ladder rung: uniform/ + adaptive/ stage dirs
        (rung / "uniform").symlink_to(ws)
        (rung / "adaptive").symlink_to(ws)
        case_file = ws / "case.json"
        if not case_file.is_file():
            case_file = ws / "adapt_params.json"
        if case_file.is_file():
            (rung / "case.json").write_text(case_file.read_text())
        out = rung / "report.html"
        proc = subprocess.run(
            [sys.executable, "-m", "validation.make_report",
             "--rung", str(rung), "--out", str(out)],
            cwd=repo_root,
            capture_output=True,
            timeout=300,
            check=False,
        )
        if proc.returncode != 0 or not out.is_file():
            raise HTTPException(
                status_code=500,
                detail=f"report generation failed: {proc.stderr.decode()[-1000:]}",
            )
        service.artifacts.save(run_id, "report.html", out.read_bytes())
    return ReportResponse(artifact="report.html")
