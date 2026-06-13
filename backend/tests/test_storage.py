"""Storage-abstraction contract tests.

The same behavioral suite runs against every implementation of each interface
(currently SQLite for metadata, local FS for artifacts). A future Postgres or
S3 implementation plugs into these classes by overriding the factory.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from app.storage import (
    ArtifactStore,
    LocalFSArtifactStore,
    Run,
    RunStatus,
    RunStore,
    SQLiteRunStore,
)


class RunStoreContract:
    """Behavioral contract every RunStore implementation must satisfy."""

    def make_store(self, tmp_path: Path) -> RunStore:
        raise NotImplementedError

    def test_create_and_get_roundtrip(self, tmp_path: Path) -> None:
        store = self.make_store(tmp_path)
        run = Run(id="r1", name="cube", config={"reynolds": 500.0})
        store.create(run)
        loaded = store.get("r1")
        assert loaded is not None
        assert loaded.id == "r1"
        assert loaded.name == "cube"
        assert loaded.status is RunStatus.CREATED
        assert loaded.config == {"reynolds": 500.0}
        assert loaded.created_at == run.created_at
        store.close()

    def test_get_missing_returns_none(self, tmp_path: Path) -> None:
        store = self.make_store(tmp_path)
        assert store.get("nope") is None
        store.close()

    def test_update_persists_status_and_artifacts(self, tmp_path: Path) -> None:
        store = self.make_store(tmp_path)
        run = Run(id="r2", name="x", config={})
        store.create(run)
        run.status = RunStatus.RUNNING
        run.artifacts["stl"] = "cube.stl"
        run.error = None
        run.touch()
        store.update(run)
        loaded = store.get("r2")
        assert loaded is not None
        assert loaded.status is RunStatus.RUNNING
        assert loaded.artifacts == {"stl": "cube.stl"}
        store.close()

    def test_list_returns_all_runs(self, tmp_path: Path) -> None:
        store = self.make_store(tmp_path)
        for i in range(3):
            store.create(Run(id=f"id{i}", name=f"run{i}", config={}))
        runs = store.list()
        assert {r.id for r in runs} == {"id0", "id1", "id2"}
        store.close()


class TestSQLiteRunStore(RunStoreContract):
    def make_store(self, tmp_path: Path) -> RunStore:
        return SQLiteRunStore(tmp_path / "runs.sqlite3")


class ArtifactStoreContract:
    """Behavioral contract every ArtifactStore implementation must satisfy."""

    def make_store(self, tmp_path: Path) -> ArtifactStore:
        raise NotImplementedError

    def test_save_read_roundtrip(self, tmp_path: Path) -> None:
        store = self.make_store(tmp_path)
        name = store.save("run1", "cube.stl", b"solid cube")
        assert name == "cube.stl"
        assert store.read("run1", "cube.stl") == b"solid cube"
        assert store.exists("run1", "cube.stl")
        assert not store.exists("run1", "absent.stl")

    def test_runs_are_isolated(self, tmp_path: Path) -> None:
        store = self.make_store(tmp_path)
        store.save("a", "f.txt", b"A")
        store.save("b", "f.txt", b"B")
        assert store.read("a", "f.txt") == b"A"
        assert store.read("b", "f.txt") == b"B"

    def test_list_reports_names_and_sizes(self, tmp_path: Path) -> None:
        store = self.make_store(tmp_path)
        store.save("run1", "a.txt", b"xx")
        store.save("run1", "sub/b.txt", b"yyy")
        infos = {a.name: a.size for a in store.list("run1")}
        assert infos == {"a.txt": 2, "sub/b.txt": 3}

    def test_workspace_is_a_real_directory(self, tmp_path: Path) -> None:
        store = self.make_store(tmp_path)
        ws = store.workspace("run1")
        assert ws.is_dir()
        # solver-style direct writes are visible through the interface
        (ws / "diagnostics.jsonl").write_text("{}\n")
        assert store.exists("run1", "diagnostics.jsonl")

    def test_resolve_blocks_path_traversal(self, tmp_path: Path) -> None:
        store = self.make_store(tmp_path)
        store.save("run1", "ok.txt", b"fine")
        assert store.resolve("run1", "ok.txt") is not None
        assert store.resolve("run1", "../run2/secret.txt") is None
        assert store.resolve("run1", "../../etc/passwd") is None
        assert store.resolve("run1", "/etc/passwd") is None
        with pytest.raises(ValueError):
            store.save("run1", "../escape.txt", b"nope")


class TestLocalFSArtifactStore(ArtifactStoreContract):
    def make_store(self, tmp_path: Path) -> ArtifactStore:
        return LocalFSArtifactStore(tmp_path / "artifacts")
