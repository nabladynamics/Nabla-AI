"""Run orchestration: the only code that ever invokes the solver core.

Lifecycle:
  POST /api/runs       -> create():  save STL, run ``nabla_solve ingest`` (status
                          meshing), read back the geometry report -> created
  POST .../start       -> start():   write case.json, queue ``nabla_solve run``
  POST .../pause       -> pause():   SIGTERM the solver (checkpoints persist)
  POST .../resume      -> resume():  restart from the latest checkpoint with the
                          remaining step budget
"""

from __future__ import annotations

import json
import re
import shlex
import subprocess
import uuid
from pathlib import Path
from typing import Any

from app.config import Settings
from app.jobs import JobRunner
from app.models import CaseConfig
from app.storage import ArtifactStore, Run, RunStatus, RunStore

_CKPT_RE = re.compile(r"checkpoint_(\d+)\.ckpt$")


class RunServiceError(Exception):
    """Orchestration error with an HTTP-ish status hint."""

    def __init__(self, message: str, status_code: int = 409) -> None:
        super().__init__(message)
        self.status_code = status_code


class RunService:
    def __init__(
        self,
        store: RunStore,
        artifacts: ArtifactStore,
        runner: JobRunner,
        settings: Settings,
    ) -> None:
        self.store = store
        self.artifacts = artifacts
        self.runner = runner
        self.settings = settings
        self._solver_argv = shlex.split(settings.solver_command)

    # -- creation + ingest ----------------------------------------------------
    def create(
        self, name: str, config: CaseConfig, stl_bytes: bytes, stl_filename: str
    ) -> tuple[Run, dict[str, Any]]:
        run = Run(id=uuid.uuid4().hex[:12], name=name, config=config.model_dump())
        self.store.create(run)

        stl_name = Path(stl_filename).name or "geometry.stl"
        self.artifacts.save(run.id, stl_name, stl_bytes)
        run.artifacts["stl"] = stl_name
        self._set_status(run, RunStatus.MESHING)

        ws = self.artifacts.workspace(run.id)
        report_name = "geometry.json"
        mesh_name = "mesh.vtu"
        argv = [
            *self._solver_argv,
            "ingest",
            str(ws / stl_name),
            "--case",
            config.type,
            "--out-vtu",
            str(ws / mesh_name),
            "--report",
            str(ws / report_name),
        ]
        try:
            proc = subprocess.run(
                argv,
                capture_output=True,
                timeout=self.settings.ingest_timeout_s,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            run.error = f"ingest failed to execute: {exc}"
            self._set_status(run, RunStatus.FAILED)
            return run, {}
        if proc.returncode != 0:
            run.error = f"ingest exited {proc.returncode}: {proc.stderr.decode()[-2000:]}"
            self._set_status(run, RunStatus.FAILED)
            return run, {}

        report: dict[str, Any] = json.loads(self.artifacts.read(run.id, report_name))
        run.artifacts["geometry_report"] = report_name
        run.artifacts["mesh_vtu"] = mesh_name
        self._set_status(run, RunStatus.CREATED)
        return run, report

    # -- lifecycle -------------------------------------------------------------
    def start(self, run_id: str, config_override: CaseConfig | None = None) -> Run:
        run = self._require(run_id)
        if run.status is not RunStatus.CREATED:
            raise RunServiceError(f"cannot start a run in status '{run.status}'")
        if config_override is not None:
            # The user confirmed/overrode the config after creation (e.g. via
            # the AI co-pilot card). Still a validated CaseConfig — nothing
            # unvalidated can reach the solver.
            run.config = config_override.model_dump()
        config = CaseConfig.model_validate(run.config)
        if config.adaptive:
            argv = self._adaptive_argv(run, config)
        else:
            case_file = self._write_case_file(run, restart_from=None, max_steps=None)
            run.artifacts["case"] = case_file.name
            argv = [*self._solver_argv, "run", str(case_file)]
        self._launch(run, argv)
        return run

    def pause(self, run_id: str) -> Run:
        run = self._require(run_id)
        if run.status is not RunStatus.RUNNING:
            raise RunServiceError(f"cannot pause a run in status '{run.status}'")
        # Status first: the worker's on_exit sees PAUSED and leaves it alone.
        self._set_status(run, RunStatus.PAUSED)
        self.runner.cancel(run_id)
        return run

    def resume(self, run_id: str) -> Run:
        run = self._require(run_id)
        if run.status is not RunStatus.PAUSED:
            raise RunServiceError(f"cannot resume a run in status '{run.status}'")
        config = CaseConfig.model_validate(run.config)
        if config.adaptive:
            raise RunServiceError(
                "adaptive runs cannot resume from checkpoints in Phase 0 — pause is a stop"
            )
        ckpt = self._latest_checkpoint(run.id)
        if ckpt is None:
            case_file = self._write_case_file(run, restart_from=None, max_steps=None)
        else:
            step, path = ckpt
            remaining = max(config.max_steps - step, 1)
            case_file = self._write_case_file(run, restart_from=path, max_steps=remaining)
        self._launch(run, [*self._solver_argv, "run", str(case_file)])
        return run

    def geometry_report(self, run_id: str) -> dict[str, Any]:
        run = self._require(run_id)
        name = run.artifacts.get("geometry_report")
        if name is None or not self.artifacts.exists(run_id, name):
            raise RunServiceError("run has no geometry report", status_code=404)
        report: dict[str, Any] = json.loads(self.artifacts.read(run_id, name))
        return report

    # -- internals --------------------------------------------------------------
    def _adaptive_argv(self, run: Run, config: CaseConfig) -> list[str]:
        ws = self.artifacts.workspace(run.id)
        argv = [
            *self._solver_argv,
            "adapt",
            "--res",
            str(config.resolution),
            "--re",
            str(config.reynolds),
            "--warmup",
            str(config.adaptive_warmup),
            "--steps",
            str(config.max_steps),
            "--run-dir",
            str(ws),
        ]
        # provenance record (the adapt CLI takes flags, not a case file)
        (ws / "adapt_params.json").write_text(
            json.dumps(config.model_dump(), indent=2, ensure_ascii=False) + "\n"
        )
        run.artifacts["case"] = "adapt_params.json"
        return argv

    def _launch(self, run: Run, argv: list[str]) -> None:
        ws = self.artifacts.workspace(run.id)
        self._set_status(run, RunStatus.RUNNING)
        try:
            self.runner.submit(run.id, argv, self._on_job_exit, log_path=ws / "solver.log")
        except RuntimeError as exc:
            self._set_status(run, RunStatus.FAILED, error=str(exc))
            raise RunServiceError(str(exc)) from exc

    def _on_job_exit(self, run_id: str, returncode: int, cancelled: bool) -> None:
        """Worker-thread callback: reconcile final status."""
        run = self.store.get(run_id)
        if run is None:
            return
        if cancelled:
            # pause() already set PAUSED; don't overwrite a user decision.
            if run.status is RunStatus.RUNNING:
                self._set_status(run, RunStatus.PAUSED)
            return
        if returncode == 0:
            self._set_status(run, RunStatus.COMPLETED)
        else:
            self._set_status(run, RunStatus.FAILED, error=f"solver exited {returncode}")

    def _write_case_file(
        self, run: Run, restart_from: Path | None, max_steps: int | None
    ) -> Path:
        config = CaseConfig.model_validate(run.config)
        ws = self.artifacts.workspace(run.id)
        payload: dict[str, Any] = {
            "type": config.type,
            "name": config.name,
            "reynolds": config.reynolds,
            "resolution": config.resolution,
            "max_steps": max_steps if max_steps is not None else config.max_steps,
            "snapshot_every": config.snapshot_every,
            "checkpoint_every": config.checkpoint_every,
            "cfl": config.cfl,
            "convection": config.convection,
            "steady_tol": config.steady_tol,
            "run_dir": str(ws),
        }
        if restart_from is not None:
            payload["restart_from"] = str(restart_from)
        case_file = ws / ("case_resume.json" if restart_from else "case.json")
        # ensure_ascii=False: the core's JSON parser consumes raw UTF-8 paths.
        case_file.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n")
        return case_file

    def _latest_checkpoint(self, run_id: str) -> tuple[int, Path] | None:
        best: tuple[int, Path] | None = None
        for info in self.artifacts.list(run_id):
            m = _CKPT_RE.search(info.name)
            if m:
                step = int(m.group(1))
                path = self.artifacts.resolve(run_id, info.name)
                if path is not None and (best is None or step > best[0]):
                    best = (step, path)
        return best

    def _set_status(self, run: Run, status: RunStatus, error: str | None = None) -> None:
        run.status = status
        if error is not None:
            run.error = error
        run.touch()
        self.store.update(run)

    def _require(self, run_id: str) -> Run:
        run = self.store.get(run_id)
        if run is None:
            raise RunServiceError(f"run not found: {run_id}", status_code=404)
        return run
