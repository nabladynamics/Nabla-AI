"""Request/response models and the spec serialization used for file-based IPC."""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class CaseConfig(BaseModel):
    """Validated solver case configuration.

    Mirrors the fields accepted by the core's ``loadCase`` (case.json). This is
    the ONLY shape that ever reaches the solver: every field is typed and
    bounded, ``extra="forbid"`` rejects unknown keys, and the run name is
    constrained to a slug — free text can never leak into solver input.
    """

    model_config = ConfigDict(extra="forbid")

    type: Literal["wall-mounted-cube", "channel", "lid-cavity"] = "wall-mounted-cube"
    name: str = Field(default="run", pattern=r"^[A-Za-z0-9._-]{1,64}$")
    reynolds: float = Field(default=500.0, gt=0, le=1e7)
    resolution: int = Field(default=6, ge=2, le=64)
    max_steps: int = Field(default=200, ge=1, le=1_000_000)
    snapshot_every: int = Field(default=20, ge=0, le=100_000)
    checkpoint_every: int = Field(default=40, ge=1, le=100_000)
    cfl: float = Field(default=0.7, gt=0, lt=1.5)
    convection: Literal["weno5", "central"] = "weno5"
    steady_tol: float = Field(default=0.0, ge=0)
    # Adaptive (physics-aware AMR) solve: launches `nabla_solve adapt`, which
    # streams diagnostics + the audited decision trail and publishes
    # adaptive_latest.vtu for the live fidelity map. max_steps then counts the
    # adaptive steps that follow `adaptive_warmup` baseline warmup steps.
    adaptive: bool = False
    adaptive_warmup: int = Field(default=25, ge=0, le=10_000)


class SimulationSpec(BaseModel):
    """A validated simulation request.

    ``to_spec`` serializes this to the core's ``key = value`` spec format. The
    field names and value semantics mirror ``nabla::SimConfig`` in
    ``core/include/nabla/config.hpp`` — that mirroring *is* the IPC contract.
    """

    nx: int = Field(default=64, ge=2, le=4096, description="grid cells in x")
    ny: int = Field(default=64, ge=2, le=4096, description="grid cells in y")
    steps: int = Field(default=100, ge=1, le=1_000_000)
    diffusivity: float = Field(default=0.1, gt=0.0)
    dt: float = Field(default=0.1, gt=0.0)
    dx: float = Field(default=1.0, gt=0.0)
    boundary_temp: float = 0.0
    initial_temp: float = 1.0

    def to_spec(self) -> str:
        """Render the core spec file body (one ``key = value`` per line)."""
        return "".join(f"{key} = {value}\n" for key, value in self.model_dump().items())


class SolveResult(BaseModel):
    """Parsed result of a solve, mirroring ``nabla::SolveResult``."""

    steps_run: int
    final_max: float
    final_mean: float
    stability: float
    stable: bool
