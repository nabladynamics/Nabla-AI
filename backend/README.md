# nabla backend

Python 3.12 FastAPI orchestration service. The middle layer of the 3-layer
architecture: it validates requests and drives the solver `core` over
**file-based IPC** — it runs no physics itself.

## Setup (uv)

```bash
uv sync                       # create .venv and install deps + dev tools
uv run uvicorn app.main:app --reload --ws wsproto --port 8000
```

Open http://localhost:8000/docs for the OpenAPI UI.

## Quality gates

```bash
uv run ruff check .           # lint
uv run mypy                   # static types (strict)
uv run pytest                 # tests
```

## Layout

| Path                  | Role                                                    |
| --------------------- | ------------------------------------------------------- |
| `app/main.py`         | FastAPI app factory (wires stores, job runner, service).|
| `app/config.py`       | Environment-driven settings (`NABLA_*`).                |
| `app/models.py`       | `CaseConfig` (validated solver config) + legacy spec.   |
| `app/storage/`        | Storage abstraction (ADR-0005): SQLite metadata + FS artifacts; Postgres/S3 slot in behind the same interfaces. |
| `app/jobs.py`         | `JobRunner` seam; `LocalJobRunner` subprocess worker.   |
| `app/services.py`     | Run orchestration: ingest, start/pause/resume (checkpoints). |
| `app/routers/runs.py` | `/api/runs` CRUD, artifacts, WS telemetry.              |
| `app/ai.py` + `routers/ai.py` | Experiment co-pilot (Anthropic API, schema-gated card). |
| `app/ipc.py`          | Parses the core's result file (IPC contract).           |

## API (Phase 0)

```
POST /api/runs                      multipart: stl file + config JSON -> geometry report
POST /api/runs/{id}/start|pause|resume
GET  /api/runs            GET /api/runs/{id}
GET  /api/runs/{id}/artifacts       GET /api/runs/{id}/artifacts/<path>
WS   /api/runs/{id}/telemetry       snapshot-on-connect + incremental diagnostics/audit lines
GET  /api/runs/{id}/fidelity-slice  2D centerplane slice of the latest octree snapshot
                                    (level + physics-model per cell; extracted server-side)
POST /api/runs/{id}/ai/ask          experiment co-pilot (needs ANTHROPIC_API_KEY)
```

Set `"adaptive": true` in the case config to launch the physics-aware AMR solve
(`nabla_solve adapt`): it streams the audited decision trail alongside
diagnostics and publishes `adaptive_latest.vtu` for the live fidelity map.
```
```

The backend is the **only** layer that touches the solver binary
(`NABLA_SOLVER_COMMAND`, default `../core/build/nabla_solve`). Run data lives
under `NABLA_DATA_DIR` (default `.nabla-data/`).
