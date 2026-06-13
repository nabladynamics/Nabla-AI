"""Smoke tests for the HTTP surface."""

from __future__ import annotations

from app.main import app
from fastapi.testclient import TestClient

client = TestClient(app)


def test_health_ok() -> None:
    response = client.get("/health")
    assert response.status_code == 200
    body = response.json()
    assert body["status"] == "ok"
    assert body["service"] == "nabla-backend"
    assert body["git_sha"]  # build provenance is always reported ("unknown" locally)


def test_render_spec() -> None:
    response = client.post("/simulations/spec", json={"nx": 32, "ny": 32, "steps": 10})
    assert response.status_code == 200
    spec = response.json()["spec"]
    assert "nx = 32" in spec
    assert "steps = 10" in spec
