"""Liveness endpoint."""

from __future__ import annotations

from fastapi import APIRouter
from pydantic import BaseModel

router = APIRouter(tags=["health"])


class Health(BaseModel):
    status: str
    service: str
    version: str
    git_sha: str


@router.get("/health")
def health() -> Health:
    """Report that the orchestration service is up."""
    from app.config import get_settings

    settings = get_settings()
    return Health(
        status="ok",
        service="nabla-backend",
        version=settings.version,
        git_sha=settings.git_sha,
    )
