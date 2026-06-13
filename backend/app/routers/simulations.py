"""Simulation orchestration endpoints.

Phase-0 surface. ``/simulations/spec`` renders a validated request into the
core's spec format — the file-based IPC payload. Actually dispatching the
``nabla_solve`` subprocess is intentionally left as the next increment so the
HTTP layer stays trivially testable without the compiled binary present.
"""

from __future__ import annotations

from fastapi import APIRouter
from pydantic import BaseModel

from app.models import SimulationSpec

router = APIRouter(prefix="/simulations", tags=["simulations"])


class RenderedSpec(BaseModel):
    spec: str


@router.post("/spec", response_model=RenderedSpec)
def render_spec(request: SimulationSpec) -> RenderedSpec:
    """Render a validated request to the core's ``key = value`` spec file body."""
    return RenderedSpec(spec=request.to_spec())
