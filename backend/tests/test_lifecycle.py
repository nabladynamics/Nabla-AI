"""Full run-lifecycle tests against a stub solver binary.

create (upload STL + config) -> start -> WebSocket telemetry -> complete,
plus pause/resume via solver checkpoints and artifact download/traversal.
"""

from __future__ import annotations

import json
import sys
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest
from app.config import Settings
from app.main import create_app
from fastapi.testclient import TestClient

STUB = Path(__file__).parent / "stub_solver.py"
STL_BYTES = b"solid cube\nendsolid cube\n"


@pytest.fixture
def client(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[TestClient]:
    monkeypatch.setenv("NABLA_STUB_STEP_SLEEP", "0.01")
    settings = Settings(
        solver_command=f"{sys.executable} {STUB}",
        data_dir=tmp_path / "data",
    )
    with TestClient(create_app(settings)) as test_client:
        yield test_client


def _create_run(client: TestClient, config: dict[str, Any]) -> dict[str, Any]:
    response = client.post(
        "/api/runs",
        files={"stl": ("cube.stl", STL_BYTES, "model/stl")},
        data={"config": json.dumps(config), "name": "cube-test"},
    )
    assert response.status_code == 201, response.text
    body: dict[str, Any] = response.json()
    return body


def _wait_for_status(client: TestClient, run_id: str, status: str, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        last = client.get(f"/api/runs/{run_id}").json()["status"]
        if last == status:
            return
        assert last != "failed", f"run failed while waiting for {status}"
        time.sleep(0.05)
    raise AssertionError(f"run never reached {status} (last: {last})")


def _diag_steps(client: TestClient, run_id: str) -> list[int]:
    response = client.get(f"/api/runs/{run_id}/artifacts/diagnostics.jsonl")
    assert response.status_code == 200
    records = [json.loads(line) for line in response.text.splitlines() if line.strip()]
    assert records[0]["event"] == "meta"  # build provenance heads every fresh stream
    assert records[0]["git_sha"]
    return [r["step"] for r in records if "step" in r]


def test_full_lifecycle_create_start_telemetry_complete(client: TestClient) -> None:
    # create: triggers ingest, returns the geometry report
    body = _create_run(client, {"max_steps": 30, "checkpoint_every": 10, "snapshot_every": 10})
    run = body["run"]
    report = body["geometry_report"]
    assert run["status"] == "created"
    assert report["cleaning"]["watertight"] is True
    assert run["artifacts"]["geometry_report"] == "geometry.json"
    run_id = run["id"]

    # start
    assert client.post(f"/api/runs/{run_id}/start").json()["status"] == "running"

    # telemetry: snapshot-on-connect + incremental diagnostics lines
    seen_streams: set[str] = set()
    diag_lines = 0
    with client.websocket_connect(f"/api/runs/{run_id}/telemetry") as ws:
        for _ in range(200):
            event = ws.receive_json()
            seen_streams.add(event["stream"])
            if event["stream"] == "diagnostics":
                if "step" in event["data"]:
                    diag_lines += 1
                else:  # the provenance meta line also flows over telemetry
                    assert event["data"]["event"] == "meta"
            if event["stream"] == "eof":
                break
        assert event["stream"] == "eof"
    assert "status" in seen_streams
    assert "audit" in seen_streams
    assert diag_lines == 30  # snapshot + increments add up to the full history

    _wait_for_status(client, run_id, "completed")

    # artifacts: list + download the final .vtu
    names = {a["name"] for a in client.get(f"/api/runs/{run_id}/artifacts").json()["artifacts"]}
    assert {"diagnostics.jsonl", "final.vtu", "geometry.json", "mesh.vtu"} <= names
    vtu = client.get(f"/api/runs/{run_id}/artifacts/final.vtu")
    assert vtu.status_code == 200
    assert b"VTKFile" in vtu.content


def test_pause_resume_uses_checkpoints(
    client: TestClient, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("NABLA_STUB_STEP_SLEEP", "0.05")
    body = _create_run(client, {"max_steps": 200, "checkpoint_every": 5})
    run_id = body["run"]["id"]
    client.post(f"/api/runs/{run_id}/start")

    # let it advance past at least one checkpoint, then pause
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        steps = _diag_steps(client, run_id) if _has_diag(client, run_id) else []
        if steps and steps[-1] >= 6:
            break
        time.sleep(0.05)
    paused = client.post(f"/api/runs/{run_id}/pause")
    assert paused.json()["status"] == "paused"
    time.sleep(0.4)  # let the worker reap the terminated solver
    steps_at_pause = _diag_steps(client, run_id)[-1]
    assert steps_at_pause < 200

    # resume: restarts from the latest checkpoint and finishes the budget
    monkeypatch.setenv("NABLA_STUB_STEP_SLEEP", "0.001")
    assert client.post(f"/api/runs/{run_id}/resume").json()["status"] == "running"
    _wait_for_status(client, run_id, "completed")
    steps = _diag_steps(client, run_id)
    assert steps[-1] == 200
    # the resumed segment continued from a checkpoint at/below the pause point
    assert any(s <= steps_at_pause for s in steps)


def _has_diag(client: TestClient, run_id: str) -> bool:
    return any(
        a["name"] == "diagnostics.jsonl"
        for a in client.get(f"/api/runs/{run_id}/artifacts").json()["artifacts"]
    )


def test_invalid_transitions_are_409(client: TestClient) -> None:
    body = _create_run(client, {"max_steps": 5})
    run_id = body["run"]["id"]
    assert client.post(f"/api/runs/{run_id}/pause").status_code == 409
    assert client.post(f"/api/runs/{run_id}/resume").status_code == 409
    client.post(f"/api/runs/{run_id}/start")
    _wait_for_status(client, run_id, "completed")
    assert client.post(f"/api/runs/{run_id}/start").status_code == 409


def test_artifact_path_traversal_is_blocked(client: TestClient) -> None:
    body = _create_run(client, {"max_steps": 5})
    run_id = body["run"]["id"]
    for path in ("../" + run_id + "/geometry.json", "../../etc/passwd", "%2e%2e/secret"):
        response = client.get(f"/api/runs/{run_id}/artifacts/{path}")
        assert response.status_code == 404, path


def test_bad_config_is_rejected(client: TestClient) -> None:
    response = client.post(
        "/api/runs",
        files={"stl": ("cube.stl", STL_BYTES, "model/stl")},
        data={"config": json.dumps({"max_steps": 5, "evil_field": "rm -rf"})},
    )
    assert response.status_code == 422  # extra=forbid: unknown keys never pass

    response = client.post(
        "/api/runs",
        files={"stl": ("cube.stl", STL_BYTES, "model/stl")},
        data={"config": json.dumps({"name": "bad name; rm -rf /"})},
    )
    assert response.status_code == 422  # slug pattern

    response = client.post("/api/runs", files={"stl": ("cube.stl", b"", "model/stl")})
    assert response.status_code == 422  # empty upload


def test_start_accepts_validated_config_override(client: TestClient) -> None:
    body = _create_run(client, {"max_steps": 100})
    run_id = body["run"]["id"]
    # user confirmed a different experiment after the copilot proposal
    override = {"max_steps": 12, "reynolds": 750.0, "name": "confirmed-card"}
    started = client.post(f"/api/runs/{run_id}/start", json={"config": override})
    assert started.status_code == 200
    _wait_for_status(client, run_id, "completed")
    assert _diag_steps(client, run_id)[-1] == 12  # override took effect
    assert client.get(f"/api/runs/{run_id}").json()["config"]["reynolds"] == 750.0

    # an invalid override never reaches the solver
    body2 = _create_run(client, {"max_steps": 5})
    bad = client.post(
        f"/api/runs/{body2['run']['id']}/start",
        json={"config": {"max_steps": 5, "shell": "rm -rf /"}},
    )
    assert bad.status_code == 422


def test_list_and_get(client: TestClient) -> None:
    body = _create_run(client, {"max_steps": 5})
    run_id = body["run"]["id"]
    runs = client.get("/api/runs").json()
    assert any(r["id"] == run_id for r in runs)
    assert client.get("/api/runs/does-not-exist").status_code == 404
