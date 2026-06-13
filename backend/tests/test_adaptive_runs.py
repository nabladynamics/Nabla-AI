"""Adaptive-run lifecycle + fidelity-slice endpoint tests (stub solver)."""

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


@pytest.fixture
def client(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[TestClient]:
    monkeypatch.setenv("NABLA_STUB_STEP_SLEEP", "0.01")
    settings = Settings(
        solver_command=f"{sys.executable} {STUB}",
        data_dir=tmp_path / "data",
    )
    with TestClient(create_app(settings)) as test_client:
        yield test_client


def _create_adaptive(client: TestClient, **overrides: Any) -> str:
    config = {"adaptive": True, "adaptive_warmup": 3, "max_steps": 5, **overrides}
    response = client.post(
        "/api/runs",
        files={"stl": ("cube.stl", b"solid cube\n", "model/stl")},
        data={"config": json.dumps(config), "name": "adaptive-test"},
    )
    assert response.status_code == 201, response.text
    run_id: str = response.json()["run"]["id"]
    return run_id


def _wait_completed(client: TestClient, run_id: str, timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = client.get(f"/api/runs/{run_id}").json()["status"]
        if status == "completed":
            return
        assert status != "failed"
        time.sleep(0.05)
    raise AssertionError("adaptive run never completed")


def test_adaptive_run_streams_both_telemetry_files(client: TestClient) -> None:
    run_id = _create_adaptive(client)
    assert client.post(f"/api/runs/{run_id}/start").json()["status"] == "running"

    streams: dict[str, int] = {"diagnostics": 0, "audit": 0}
    meta_lines = 0
    with client.websocket_connect(f"/api/runs/{run_id}/telemetry") as ws:
        for _ in range(400):
            event = ws.receive_json()
            if event["stream"] in streams:
                if event["data"].get("event") == "meta":  # provenance header line
                    meta_lines += 1
                else:
                    streams[event["stream"]] += 1
            if event["stream"] == "eof":
                break
    assert meta_lines == 2  # one per stream (diagnostics + audit)
    assert streams["diagnostics"] == 3 + 5  # warmup + adaptive steps
    assert streams["audit"] >= 5 * 8  # the per-step audit event bundle
    _wait_completed(client, run_id)


def test_fidelity_slice_endpoint(client: TestClient) -> None:
    run_id = _create_adaptive(client)
    client.post(f"/api/runs/{run_id}/start")
    _wait_completed(client, run_id)

    response = client.get(f"/api/runs/{run_id}/fidelity-slice", params={"axis": "z", "frac": 0.5})
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["source"] == "adaptive_latest.vtu"
    assert len(body["cells"]) >= 3  # the z-plane cuts through the stub cells
    cell = body["cells"][0]
    assert {"x", "y", "w", "h", "level", "mode"} <= set(cell)
    assert body["mode_counts"].get("SOLID") == 1  # mask=2 cell classified solid
    assert "WAKE_SHEAR" in body["mode_counts"]

    # y-axis slice also works, frac is clamped
    assert client.get(
        f"/api/runs/{run_id}/fidelity-slice", params={"axis": "y", "frac": 9.0}
    ).status_code == 200

    # unknown run -> 404; run without snapshots -> 404
    assert client.get("/api/runs/nope/fidelity-slice").status_code == 404


def test_adaptive_pause_is_stop_resume_rejected(
    client: TestClient, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("NABLA_STUB_STEP_SLEEP", "0.2")
    run_id = _create_adaptive(client, max_steps=50)
    client.post(f"/api/runs/{run_id}/start")
    time.sleep(0.5)
    assert client.post(f"/api/runs/{run_id}/pause").json()["status"] == "paused"
    response = client.post(f"/api/runs/{run_id}/resume")
    assert response.status_code == 409
    assert "adaptive" in response.json()["detail"]
