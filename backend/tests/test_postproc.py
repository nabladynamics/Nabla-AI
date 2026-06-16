"""Post-simulation endpoint tests: field bundles, analysis, exports, report."""

from __future__ import annotations

import base64
import json
import struct
import sys
import time
import zipfile
from collections.abc import Iterator
from io import BytesIO
from pathlib import Path

import pytest
from app.config import Settings
from app.main import create_app
from fastapi.testclient import TestClient

STUB = Path(__file__).parent / "stub_solver.py"
REPO_ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def client(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[TestClient]:
    monkeypatch.setenv("NABLA_STUB_STEP_SLEEP", "0.002")
    settings = Settings(
        solver_command=f"{sys.executable} {STUB}",
        data_dir=tmp_path / "data",
        repo_root=REPO_ROOT,
    )
    with TestClient(create_app(settings)) as test_client:
        yield test_client


def _completed_run(client: TestClient) -> str:
    response = client.post(
        "/api/runs",
        files={"stl": ("cube.stl", b"solid cube\n", "model/stl")},
        data={"config": json.dumps({"max_steps": 24, "snapshot_every": 8, "reynolds": 500})},
    )
    run_id: str = response.json()["run"]["id"]
    client.post(f"/api/runs/{run_id}/start")
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if client.get(f"/api/runs/{run_id}").json()["status"] == "completed":
            return run_id
        time.sleep(0.05)
    raise AssertionError("run never completed")


def _decode(b64: str) -> list[float]:
    raw = base64.b64decode(b64)
    return list(struct.unpack(f"<{len(raw) // 4}f", raw))


def test_snapshots_and_field_bundle(client: TestClient) -> None:
    run_id = _completed_run(client)
    snaps = client.get(f"/api/runs/{run_id}/snapshots").json()
    assert snaps["steps"] == [8, 16, 24]
    assert snaps["has_final"] is True

    bundle = client.get(f"/api/runs/{run_id}/field", params={"step": -1}).json()
    assert bundle["dims"] == [10, 4, 4]
    assert {"u", "v", "w", "p", "speed", "ke", "vort", "q", "solid"} <= set(bundle["fields"])
    u = _decode(bundle["fields"]["u"])
    assert len(u) == 10 * 4 * 4
    assert min(u) < 0 < max(u)  # reverse-flow pocket survives the round-trip
    assert bundle["ranges"]["q"][1] > 0  # Q-criterion computed server-side

    # decimation halves dims (rounded up)
    slim = client.get(f"/api/runs/{run_id}/field", params={"step": 8, "stride": 2}).json()
    assert slim["dims"] == [5, 2, 2]
    assert client.get(f"/api/runs/{run_id}/field", params={"step": 999}).status_code == 404


def test_field_falls_back_to_latest_snapshot_while_running(client: TestClient) -> None:
    # While a run is still in progress there is no final.vtu yet; requesting the
    # final field (step=-1) must fall back to the most recent snapshot so the
    # post-view stays populated instead of 404-ing on "snapshot not found".
    run_id = _completed_run(client)
    ws = client.app.state.service.artifacts.workspace(run_id)  # type: ignore[attr-defined]
    (ws / "final.vtu").unlink()

    response = client.get(f"/api/runs/{run_id}/field", params={"step": -1})
    assert response.status_code == 200, response.text
    bundle = response.json()
    assert bundle["step"] == 24  # latest snapshot, not the (missing) final
    assert bundle["source"] == "snapshot_24.vtu"

    # No snapshots at all -> still a clean 404.
    for snap in ws.glob("snapshot_*.vtu"):
        snap.unlink()
    assert client.get(f"/api/runs/{run_id}/field", params={"step": -1}).status_code == 404


def test_analysis_endpoint(client: TestClient) -> None:
    run_id = _completed_run(client)
    body = client.get(f"/api/runs/{run_id}/analysis", params={"window": 0.5}).json()
    assert body["forces"]["samples"] > 0
    assert body["spectrum"] is not None
    rec = body["recirculation"]
    assert rec is not None
    assert rec["reverse_flow_volume_over_h3"] > 0
    assert rec["cores"] == [] or any(
        core["label"].startswith(("arch", "horseshoe")) for core in rec["cores"]
    )
    metrics = {row["metric"]: row["status"] for row in body["comparison"]}
    assert "cd_mean" in metrics  # registry rung found -> NO-REF until digitized
    assert body["reference_re"] == 500
    assert body["audit"] is not None  # the stub run emits an audit stream


def test_exports_csv_and_zip(client: TestClient) -> None:
    run_id = _completed_run(client)
    csv_response = client.get(f"/api/runs/{run_id}/forces.csv")
    assert csv_response.status_code == 200
    lines = csv_response.text.splitlines()
    assert lines[0].startswith("step,t,dt,cd,cl,fd,fl")
    assert len(lines) == 1 + 24

    zip_response = client.get(f"/api/runs/{run_id}/bundle.zip", params={"suffix": ".vtu"})
    assert zip_response.status_code == 200
    archive = zipfile.ZipFile(BytesIO(zip_response.content))
    names = set(archive.namelist())
    assert {"final.vtu", "snapshot_8.vtu"} <= names


def test_report_generation_via_validation_harness(client: TestClient) -> None:
    run_id = _completed_run(client)
    response = client.post(f"/api/runs/{run_id}/report")
    assert response.status_code == 200, response.text
    artifact = response.json()["artifact"]
    report = client.get(f"/api/runs/{run_id}/artifacts/{artifact}")
    assert report.status_code == 200
    assert b"Phase 0 Validation Report" in report.content
