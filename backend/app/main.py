"""FastAPI application factory for the Nabla AI orchestration service.

The middle layer of the 3-layer architecture (frontend / backend / core): it
validates requests, stores run metadata + artifacts (ADR-0005), and drives the
solver core as a managed subprocess over file-based IPC. The backend is the
ONLY component that ever touches the solver binary; it contains no physics.
"""

from __future__ import annotations

from collections.abc import AsyncIterator
from contextlib import asynccontextmanager

from fastapi import FastAPI

from app.ai import ExperimentCopilot
from app.config import Settings, get_settings
from app.jobs import LocalJobRunner
from app.routers import ai, health, postproc, runs, simulations
from app.services import RunService
from app.storage import LocalFSArtifactStore, SQLiteRunStore


@asynccontextmanager
async def _lifespan(app: FastAPI) -> AsyncIterator[None]:
    yield
    app.state.runner.shutdown()
    app.state.store.close()


def create_app(settings: Settings | None = None) -> FastAPI:
    """Build and configure the FastAPI application."""
    settings = settings or get_settings()
    settings.data_dir.mkdir(parents=True, exist_ok=True)

    store = SQLiteRunStore(settings.data_dir / "runs.sqlite3")
    artifacts = LocalFSArtifactStore(settings.data_dir / "artifacts")
    runner = LocalJobRunner()
    service = RunService(store=store, artifacts=artifacts, runner=runner, settings=settings)

    app = FastAPI(
        title="Nabla AI Orchestration API",
        version=settings.version,
        summary="Physics-adaptive CFD platform — orchestration layer",
        lifespan=_lifespan,
    )
    app.state.settings = settings
    app.state.store = store
    app.state.artifacts = artifacts
    app.state.runner = runner
    app.state.service = service
    app.state.copilot = ExperimentCopilot(model=settings.anthropic_model)

    app.include_router(health.router)
    # also reachable same-origin through the frontend proxy (/api/*)
    app.include_router(health.router, prefix="/api", include_in_schema=False)
    app.include_router(simulations.router)  # legacy demo endpoint
    app.include_router(runs.router, prefix="/api")
    app.include_router(ai.router, prefix="/api")
    app.include_router(postproc.router, prefix="/api")
    return app


app = create_app()
