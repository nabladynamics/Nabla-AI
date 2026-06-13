"""Job execution seam (ADR-0005).

The solver runs as a subprocess managed by a small single-machine worker.
:class:`JobRunner` is the interface orchestration code depends on; a
distributed queue (Celery/SQS/...) replaces :class:`LocalJobRunner` behind it
without touching call sites.
"""

from __future__ import annotations

import queue
import subprocess
import threading
from abc import ABC, abstractmethod
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

# (run_id, returncode, cancelled) — invoked from the worker thread.
OnExit = Callable[[str, int, bool], None]


@dataclass
class _Job:
    run_id: str
    argv: list[str]
    log_path: Path | None
    on_exit: OnExit


class JobRunner(ABC):
    @abstractmethod
    def submit(
        self, run_id: str, argv: list[str], on_exit: OnExit, log_path: Path | None = None
    ) -> None:
        """Queue a job. ``on_exit`` always fires exactly once per submit."""

    @abstractmethod
    def cancel(self, run_id: str) -> bool:
        """Cancel a queued or running job (SIGTERM). False if not active."""

    @abstractmethod
    def is_active(self, run_id: str) -> bool: ...

    @abstractmethod
    def shutdown(self) -> None: ...


class LocalJobRunner(JobRunner):
    """Single worker thread + FIFO queue. Phase-0 single-machine execution."""

    def __init__(self) -> None:
        self._queue: queue.Queue[_Job | None] = queue.Queue()
        self._lock = threading.Lock()
        # run_id -> Popen (running) or None (still queued)
        self._active: dict[str, subprocess.Popen[bytes] | None] = {}
        self._cancelled: set[str] = set()
        self._thread = threading.Thread(target=self._worker, daemon=True, name="nabla-jobs")
        self._thread.start()

    def submit(
        self, run_id: str, argv: list[str], on_exit: OnExit, log_path: Path | None = None
    ) -> None:
        with self._lock:
            if run_id in self._active:
                raise RuntimeError(f"run {run_id} already has an active job")
            self._active[run_id] = None
            self._cancelled.discard(run_id)
        self._queue.put(_Job(run_id, list(argv), log_path, on_exit))

    def cancel(self, run_id: str) -> bool:
        with self._lock:
            if run_id not in self._active:
                return False
            self._cancelled.add(run_id)
            proc = self._active[run_id]
            if proc is not None and proc.poll() is None:
                proc.terminate()
        return True

    def is_active(self, run_id: str) -> bool:
        with self._lock:
            return run_id in self._active

    def shutdown(self) -> None:
        self._queue.put(None)
        with self._lock:
            for proc in self._active.values():
                if proc is not None and proc.poll() is None:
                    proc.terminate()
        self._thread.join(timeout=10.0)

    # -- worker thread -------------------------------------------------------
    def _worker(self) -> None:
        while True:
            job = self._queue.get()
            if job is None:
                return
            self._run_one(job)

    def _run_one(self, job: _Job) -> None:
        with self._lock:
            cancelled_early = job.run_id in self._cancelled
            if cancelled_early:
                self._cancelled.discard(job.run_id)
                self._active.pop(job.run_id, None)
        if cancelled_early:
            job.on_exit(job.run_id, -15, True)
            return

        if job.log_path is not None:
            with open(job.log_path, "ab") as log:
                self._launch_and_wait(job, log)
        else:
            self._launch_and_wait(job, None)

    def _launch_and_wait(self, job: _Job, log: BinaryIO | None) -> None:
        sink: BinaryIO | int = log if log is not None else subprocess.DEVNULL
        try:
            proc = subprocess.Popen(job.argv, stdout=sink, stderr=sink)
        except OSError as exc:
            if log is not None:
                log.write(f"failed to launch: {exc}\n".encode())
            with self._lock:
                self._active.pop(job.run_id, None)
                self._cancelled.discard(job.run_id)
            job.on_exit(job.run_id, 127, False)
            return

        with self._lock:
            self._active[job.run_id] = proc
            if job.run_id in self._cancelled:  # cancel raced the launch
                proc.terminate()
        returncode = proc.wait()
        with self._lock:
            cancelled = job.run_id in self._cancelled
            self._cancelled.discard(job.run_id)
            self._active.pop(job.run_id, None)
        job.on_exit(job.run_id, returncode, cancelled)
