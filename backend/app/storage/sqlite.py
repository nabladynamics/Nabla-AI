"""SQLite implementation of RunStore (Phase 0).

Standard library ``sqlite3`` with WAL mode and a process-wide lock — adequate
for a single-machine orchestration service. A Postgres/Supabase implementation
replaces this class behind the same interface (ADR-0005).
"""

from __future__ import annotations

import json
import sqlite3
import threading
from datetime import datetime
from pathlib import Path
from typing import Any

from app.storage.base import Run, RunStatus, RunStore

_SCHEMA = """
CREATE TABLE IF NOT EXISTS runs (
    id         TEXT PRIMARY KEY,
    name       TEXT NOT NULL,
    status     TEXT NOT NULL,
    config     TEXT NOT NULL,
    artifacts  TEXT NOT NULL,
    error      TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
)
"""


class SQLiteRunStore(RunStore):
    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(str(path), check_same_thread=False)
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute(_SCHEMA)
        self._conn.commit()

    def create(self, run: Run) -> None:
        with self._lock:
            self._conn.execute(
                "INSERT INTO runs VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                self._to_row(run),
            )
            self._conn.commit()

    def get(self, run_id: str) -> Run | None:
        with self._lock:
            cur = self._conn.execute("SELECT * FROM runs WHERE id = ?", (run_id,))
            row = cur.fetchone()
        return self._from_row(row) if row is not None else None

    def list(self) -> list[Run]:
        with self._lock:
            cur = self._conn.execute("SELECT * FROM runs ORDER BY created_at DESC, id")
            rows = cur.fetchall()
        return [self._from_row(row) for row in rows]

    def update(self, run: Run) -> None:
        with self._lock:
            self._conn.execute(
                "UPDATE runs SET name=?, status=?, config=?, artifacts=?, error=?, "
                "created_at=?, updated_at=? WHERE id=?",
                (*self._to_row(run)[1:], run.id),
            )
            self._conn.commit()

    def close(self) -> None:
        with self._lock:
            self._conn.close()

    @staticmethod
    def _to_row(run: Run) -> tuple[str, str, str, str, str, str | None, str, str]:
        return (
            run.id,
            run.name,
            run.status.value,
            json.dumps(run.config, ensure_ascii=False),
            json.dumps(run.artifacts, ensure_ascii=False),
            run.error,
            run.created_at.isoformat(),
            run.updated_at.isoformat(),
        )

    @staticmethod
    def _from_row(row: tuple[Any, ...]) -> Run:
        return Run(
            id=row[0],
            name=row[1],
            status=RunStatus(row[2]),
            config=json.loads(row[3]),
            artifacts=json.loads(row[4]),
            error=row[5],
            created_at=datetime.fromisoformat(row[6]),
            updated_at=datetime.fromisoformat(row[7]),
        )
