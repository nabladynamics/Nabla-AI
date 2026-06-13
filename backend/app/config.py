"""Runtime configuration, loaded from the environment (prefix ``NABLA_``)."""

from __future__ import annotations

from functools import lru_cache
from pathlib import Path

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_prefix="NABLA_",
        env_file=".env",
        extra="ignore",
    )

    version: str = "0.1.0"

    # Git SHA of the deployed backend build ("unknown" outside CI/containers;
    # container builds inject it via the NABLA_GIT_SHA build arg). The solver
    # core stamps its own SHA into every diagnostics file it writes.
    git_sha: str = "unknown"

    # Command used to invoke the solver core CLI. May contain arguments
    # (it is shlex-split), e.g. "python3 tests/stub_solver.py" in tests.
    # The backend is the ONLY layer that ever touches this binary.
    solver_command: str = "../core/build/nabla_solve"

    # Root for run metadata (SQLite) and artifacts (filesystem store).
    data_dir: Path = Path(".nabla-data")

    # Wall-clock limit for the synchronous geometry-ingest step.
    ingest_timeout_s: float = 300.0

    # AI co-pilot. The Anthropic API key is read by the SDK from the standard
    # ANTHROPIC_API_KEY environment variable (never stored in settings).
    anthropic_model: str = "claude-opus-4-8"

    # Repository root: locates the validation harness (report generation) and
    # the reference registry. Default assumes the service runs from backend/.
    repo_root: Path = Path("..")

    # CORS allow-list for the browser frontend (comma-separated origins, or "*").
    # Local dev / compose serve the SPA same-origin (Vite proxy / nginx) so CORS
    # is moot there; this matters once the frontend is on Vercel and the backend
    # on Railway (cross-origin). Default "*" for the Phase-0 demo — LOCK THIS
    # DOWN to the real Vercel domain (e.g. "https://nabla.vercel.app") via
    # NABLA_ALLOWED_ORIGINS once it is known.
    allowed_origins: str = "*"

    @property
    def cors_origins(self) -> list[str]:
        """Parsed CORS allow-list. "*" stays a single wildcard entry."""
        return [o.strip() for o in self.allowed_origins.split(",") if o.strip()]


@lru_cache
def get_settings() -> Settings:
    """Return a cached Settings instance."""
    return Settings()
