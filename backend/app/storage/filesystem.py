"""Local-filesystem implementation of ArtifactStore (Phase 0).

Layout: ``<root>/<run_id>/<relative artifact name>``. An S3 implementation
replaces this class behind the same interface, staging the run workspace to a
local scratch directory for the solver's file-based IPC (ADR-0005).
"""

from __future__ import annotations

from pathlib import Path

from app.storage.base import ArtifactInfo, ArtifactStore


class LocalFSArtifactStore(ArtifactStore):
    def __init__(self, root: Path) -> None:
        self._root = root
        self._root.mkdir(parents=True, exist_ok=True)

    def workspace(self, run_id: str) -> Path:
        path = self._root / run_id
        path.mkdir(parents=True, exist_ok=True)
        return path

    def save(self, run_id: str, name: str, data: bytes) -> str:
        path = self._safe_path(run_id, name)
        if path is None:
            raise ValueError(f"invalid artifact name: {name!r}")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return name

    def read(self, run_id: str, name: str) -> bytes:
        path = self._safe_path(run_id, name)
        if path is None or not path.is_file():
            raise FileNotFoundError(f"{run_id}/{name}")
        return path.read_bytes()

    def exists(self, run_id: str, name: str) -> bool:
        path = self._safe_path(run_id, name)
        return path is not None and path.is_file()

    def list(self, run_id: str) -> list[ArtifactInfo]:
        ws = self.workspace(run_id)
        out = [
            ArtifactInfo(name=str(p.relative_to(ws)), size=p.stat().st_size)
            for p in ws.rglob("*")
            if p.is_file()
        ]
        return sorted(out, key=lambda a: a.name)

    def resolve(self, run_id: str, name: str) -> Path | None:
        path = self._safe_path(run_id, name)
        if path is None or not path.is_file():
            return None
        return path

    def _safe_path(self, run_id: str, name: str) -> Path | None:
        """Resolve name inside the run dir; None when it escapes (traversal)."""
        ws = self.workspace(run_id).resolve()
        candidate = (ws / name).resolve()
        if candidate == ws or not candidate.is_relative_to(ws):
            return None
        return candidate
