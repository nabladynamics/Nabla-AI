"""Tests for the file-based IPC (de)serialization contract."""

from __future__ import annotations

from app.ipc import parse_result
from app.models import SimulationSpec


def test_spec_serialization_roundtrips_known_keys() -> None:
    spec = SimulationSpec(nx=32, ny=48, steps=10, diffusivity=0.2)
    text = spec.to_spec()
    assert "nx = 32" in text
    assert "ny = 48" in text
    assert "steps = 10" in text
    assert "diffusivity = 0.2" in text


def test_parse_result_reads_core_output() -> None:
    text = (
        "# nabla_solve result\n"
        "steps_run = 150\n"
        "final_max = 1\n"
        "final_mean = 0.771974\n"
        "stability = 0.01\n"
        "stable = true\n"
    )
    result = parse_result(text)
    assert result.steps_run == 150
    assert result.final_mean == 0.771974
    assert result.stable is True
