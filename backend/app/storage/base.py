"""Storage interfaces and the Run model (ADR-0005).

Two narrow interfaces decouple orchestration from persistence:

- :class:`RunStore`     — CRUD for run metadata (SQLite now; Postgres later).
- :class:`ArtifactStore`— binary artifacts: STL, meshes, snapshots,
  checkpoints, reports (local filesystem now; S3 later).

The solver communicates via files (file-based IPC), so every run owns a
*workspace* — a real local directory the solver reads and writes. A remote
artifact store implements :meth:`ArtifactStore.workspace` by staging to a
local scratch directory and syncing; call sites never assume more than the
interface.
"""

from __future__ import annotations

import enum
from abc import ABC, abstractmethod
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from pydantic import BaseModel, Field


class RunStatus(enum.StrEnum):
    CREATED = "created"
    MESHING = "meshing"
    RUNNING = "running"
    PAUSED = "paused"
    COMPLETED = "completed"
    FAILED = "failed"


def utcnow() -> datetime:
    return datetime.now(UTC)


class Run(BaseModel):
    """A simulation run: id, case config, status, timestamps, artifact paths."""

    id: str
    name: str
    status: RunStatus = RunStatus.CREATED
    config: dict[str, Any] = Field(default_factory=dict)
    artifacts: dict[str, str] = Field(default_factory=dict)
    error: str | None = None
    created_at: datetime = Field(default_factory=utcnow)
    updated_at: datetime = Field(default_factory=utcnow)

    def touch(self) -> None:
        self.updated_at = utcnow()


class ArtifactInfo(BaseModel):
    name: str  # path relative to the run's workspace
    size: int


class RunStore(ABC):
    """Run-metadata persistence. Implementations must be thread-safe."""

    @abstractmethod
    def create(self, run: Run) -> None: ...

    @abstractmethod
    def get(self, run_id: str) -> Run | None: ...

    @abstractmethod
    def list(self) -> list[Run]:
        """All runs, newest first."""

    @abstractmethod
    def update(self, run: Run) -> None:
        """Persist the given run (matched by id)."""

    @abstractmethod
    def close(self) -> None: ...


class ArtifactStore(ABC):
    """Binary artifact persistence, keyed by (run_id, relative name)."""

    @abstractmethod
    def workspace(self, run_id: str) -> Path:
        """A local directory for the run's file-based solver IPC.

        Always exists after this call. Remote implementations stage/sync.
        """

    @abstractmethod
    def save(self, run_id: str, name: str, data: bytes) -> str:
        """Store an artifact; returns the relative name it was stored under."""

    @abstractmethod
    def read(self, run_id: str, name: str) -> bytes: ...

    @abstractmethod
    def exists(self, run_id: str, name: str) -> bool: ...

    @abstractmethod
    def list(self, run_id: str) -> list[ArtifactInfo]:
        """Every artifact for the run, sorted by name."""

    @abstractmethod
    def resolve(self, run_id: str, name: str) -> Path | None:
        """Local path for serving, or None if absent or outside the run's
        directory (path-traversal guard)."""
