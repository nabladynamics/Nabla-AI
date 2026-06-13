"""Run lifecycle endpoints: CRUD, artifacts, and WebSocket telemetry."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Annotated, Any, cast

from fastapi import APIRouter, Depends, File, Form, HTTPException, Request, UploadFile
from fastapi.responses import FileResponse
from fastapi.websockets import WebSocket, WebSocketDisconnect
from pydantic import BaseModel, ValidationError

from app.models import CaseConfig
from app.services import RunService, RunServiceError
from app.slices import SliceAxis, SliceResponse, fidelity_slice
from app.storage import ArtifactInfo, Run, RunStatus

router = APIRouter(prefix="/runs", tags=["runs"])

_TERMINAL = {RunStatus.COMPLETED, RunStatus.FAILED}
_TELEMETRY_STREAMS = ("diagnostics.jsonl", "audit.jsonl")
_POLL_SECONDS = 0.25


def get_service(request: Request) -> RunService:
    return cast(RunService, request.app.state.service)


ServiceDep = Annotated[RunService, Depends(get_service)]


class RunCreateResponse(BaseModel):
    run: Run
    geometry_report: dict[str, Any]


class ArtifactListResponse(BaseModel):
    artifacts: list[ArtifactInfo]


class StartRequest(BaseModel):
    config: CaseConfig | None = None


def _raise(exc: RunServiceError) -> HTTPException:
    return HTTPException(status_code=exc.status_code, detail=str(exc))


@router.post("", response_model=RunCreateResponse, status_code=201)
def create_run(
    service: ServiceDep,
    stl: Annotated[UploadFile, File(description="STL geometry file")],
    config: Annotated[str, Form(description="Case config JSON (CaseConfig schema)")] = "{}",
    name: Annotated[str, Form()] = "",
) -> RunCreateResponse:
    """Upload an STL + case config; runs geometry ingest; returns the report."""
    try:
        cfg = CaseConfig.model_validate(json.loads(config))
    except json.JSONDecodeError as exc:
        raise HTTPException(status_code=422, detail=f"config is not valid JSON: {exc}") from exc
    except ValidationError as exc:
        raise HTTPException(status_code=422, detail=exc.errors(include_url=False)) from exc

    data = stl.file.read()
    if not data:
        raise HTTPException(status_code=422, detail="empty STL upload")

    run, report = service.create(
        name=name or cfg.name,
        config=cfg,
        stl_bytes=data,
        stl_filename=stl.filename or "geometry.stl",
    )
    if run.status is RunStatus.FAILED:
        raise HTTPException(status_code=422, detail=run.error or "geometry ingest failed")
    return RunCreateResponse(run=run, geometry_report=report)


@router.get("", response_model=list[Run])
def list_runs(service: ServiceDep) -> list[Run]:
    return service.store.list()


@router.get("/{run_id}", response_model=Run)
def get_run(service: ServiceDep, run_id: str) -> Run:
    run = service.store.get(run_id)
    if run is None:
        raise HTTPException(status_code=404, detail=f"run not found: {run_id}")
    return run


@router.post("/{run_id}/start", response_model=Run)
def start_run(service: ServiceDep, run_id: str, body: StartRequest | None = None) -> Run:
    """Start the run; an optional validated config override (the user-confirmed
    experiment definition) replaces the config stored at creation time."""
    try:
        return service.start(run_id, config_override=body.config if body else None)
    except RunServiceError as exc:
        raise _raise(exc) from exc


@router.post("/{run_id}/pause", response_model=Run)
def pause_run(service: ServiceDep, run_id: str) -> Run:
    try:
        return service.pause(run_id)
    except RunServiceError as exc:
        raise _raise(exc) from exc


@router.post("/{run_id}/resume", response_model=Run)
def resume_run(service: ServiceDep, run_id: str) -> Run:
    """Resume a paused run from the latest solver checkpoint."""
    try:
        return service.resume(run_id)
    except RunServiceError as exc:
        raise _raise(exc) from exc


# -- artifacts -----------------------------------------------------------------
@router.get("/{run_id}/artifacts", response_model=ArtifactListResponse)
def list_artifacts(service: ServiceDep, run_id: str) -> ArtifactListResponse:
    if service.store.get(run_id) is None:
        raise HTTPException(status_code=404, detail=f"run not found: {run_id}")
    return ArtifactListResponse(artifacts=service.artifacts.list(run_id))


@router.get("/{run_id}/artifacts/{artifact_path:path}")
def download_artifact(service: ServiceDep, run_id: str, artifact_path: str) -> FileResponse:
    if service.store.get(run_id) is None:
        raise HTTPException(status_code=404, detail=f"run not found: {run_id}")
    resolved = service.artifacts.resolve(run_id, artifact_path)
    if resolved is None:  # absent OR path traversal — same answer for both
        raise HTTPException(status_code=404, detail=f"artifact not found: {artifact_path}")
    return FileResponse(resolved, filename=Path(artifact_path).name)


# -- fidelity map slice -----------------------------------------------------------
@router.get("/{run_id}/fidelity-slice", response_model=SliceResponse)
def get_fidelity_slice(
    service: ServiceDep,
    run_id: str,
    axis: SliceAxis = "z",
    frac: float = 0.5,
) -> SliceResponse:
    """2D centerplane slice of the latest octree snapshot, extracted server-side
    (refinement level + physics-model label per cell). The browser never loads
    full 3D fields for the live fidelity map."""
    if service.store.get(run_id) is None:
        raise HTTPException(status_code=404, detail=f"run not found: {run_id}")
    response = fidelity_slice(service.artifacts.workspace(run_id), axis, frac)
    if response is None:
        raise HTTPException(status_code=404, detail="no mesh snapshot available yet")
    return response


# -- telemetry (WebSocket) -------------------------------------------------------
class _Tail:
    """Incremental line reader over a (possibly growing) text file."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.offset = 0

    def read_new_lines(self) -> list[str]:
        if not self.path.exists():
            return []
        size = self.path.stat().st_size
        if size <= self.offset:
            return []
        with open(self.path, "rb") as f:
            f.seek(self.offset)
            chunk = f.read(size - self.offset)
        # only consume complete lines; a partial tail stays for the next poll
        last_newline = chunk.rfind(b"\n")
        if last_newline < 0:
            return []
        self.offset += last_newline + 1
        return chunk[: last_newline + 1].decode("utf-8", errors="replace").splitlines()


def _as_event(stream: str, line: str) -> dict[str, Any]:
    try:
        data: Any = json.loads(line)
    except json.JSONDecodeError:
        data = line
    return {"stream": stream, "data": data}


@router.websocket("/{run_id}/telemetry")
async def telemetry(websocket: WebSocket) -> None:
    """Tails diagnostics.jsonl and audit.jsonl: full snapshot on connect, then
    incremental lines as the solver appends. Also emits status transitions."""
    run_id = websocket.path_params["run_id"]
    service = cast(RunService, websocket.app.state.service)
    run = service.store.get(run_id)
    if run is None:
        await websocket.close(code=4404)
        return

    await websocket.accept()
    workspace = service.artifacts.workspace(run_id)
    tails = {name.split(".")[0]: _Tail(workspace / name) for name in _TELEMETRY_STREAMS}

    await websocket.send_json({"stream": "status", "data": run.model_dump(mode="json")})
    last_status = run.status
    drained_after_terminal = 0
    try:
        while True:
            sent = 0
            for stream, tail in tails.items():
                for line in tail.read_new_lines():  # snapshot first, increments after
                    await websocket.send_json(_as_event(stream, line))
                    sent += 1
            current = service.store.get(run_id)
            if current is not None and current.status != last_status:
                last_status = current.status
                await websocket.send_json(
                    {"stream": "status", "data": current.model_dump(mode="json")}
                )
            if current is not None and current.status in _TERMINAL:
                drained_after_terminal = drained_after_terminal + 1 if sent == 0 else 0
                if drained_after_terminal >= 2:  # terminal + fully drained
                    await websocket.send_json({"stream": "eof", "data": current.status})
                    break
            await asyncio.sleep(_POLL_SECONDS)
    except WebSocketDisconnect:
        return
    await websocket.close()
