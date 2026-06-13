"""Storage abstraction (ADR-0005): run metadata + artifact stores.

Call sites depend only on the interfaces in ``base``; the Phase-0
implementations are SQLite (metadata) and the local filesystem (artifacts).
Postgres/Supabase and S3 implementations slot in behind the same interfaces.
"""

from app.storage.base import ArtifactInfo, ArtifactStore, Run, RunStatus, RunStore
from app.storage.filesystem import LocalFSArtifactStore
from app.storage.sqlite import SQLiteRunStore

__all__ = [
    "ArtifactInfo",
    "ArtifactStore",
    "LocalFSArtifactStore",
    "Run",
    "RunStatus",
    "RunStore",
    "SQLiteRunStore",
]
