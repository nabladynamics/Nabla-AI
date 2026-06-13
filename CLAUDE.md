# Nabla AI — Architecture & Working Agreement

Nabla AI is a **physics-adaptive CFD platform**. This file captures the
architecture rules that every change must respect. They are not suggestions.
The rationale lives in [ADR-0001](docs/adr/0001-local-first-client-server-architecture.md).

## The four rules

1. **Three-layer separation.** Frontend → Backend → Core. Each layer talks only
   to its immediate neighbor.
   - **`/frontend`** (React + TS): presentation only. Talks to the backend over
     HTTP. **Never** calls the core or touches solver files.
   - **`/backend`** (Python + FastAPI): orchestration, validation, job
     management. The **only** layer allowed to invoke the core.
   - **`/core`** (C++20): headless numerics. No UI, no HTTP server, no knowledge
     of the web layers.

2. **File-based IPC between core and backend.** The backend writes a plain-text
   `key = value` **spec file**, invokes `nabla_solve --input <spec> --output
   <result>`, and reads back a `key = value` **result file**. That text contract
   is the *only* coupling between the two layers.
   - Contract definition: [`core/include/nabla/io.hpp`](core/include/nabla/io.hpp)
     and [`core/include/nabla/config.hpp`](core/include/nabla/config.hpp).
   - Backend side: [`backend/app/models.py`](backend/app/models.py) (spec out)
     and [`backend/app/ipc.py`](backend/app/ipc.py) (result in).
   - Keep field names and semantics in lockstep across both sides when you change
     the contract.

3. **No GPL in the core.** `/core` and anything it links into the shipped
   `nabla_core` / `nabla_solve` artifacts may depend **only** on permissively
   licensed code: **BSD / MIT / Apache / Boost (BSL)**. No GPL/LGPL/AGPL — ever.
   The solver is proprietary IP that must remain freely re-licensable.
   - Test-only dependencies that are never linked into the shipped artifacts
     (e.g. Catch2, BSL-1.0) are acceptable.

4. **Everything runs locally (Phase 0).** The whole product runs on one machine
   with no cloud account. `scripts/dev.sh` builds the core and starts the backend
   (`:8000`) and frontend (`:5173`). The solver core is **never** shipped to
   clients as a raw binary — it stays behind the backend (trade-secret boundary).
   The same three processes containerize unchanged for hosted SaaS and on-prem.

## Repository map

```
core/        C++20 solver. Targets: nabla_core (lib), nabla_solve (CLI), nabla_mesh_demo, nabla_tests (Catch2).
             mesh/      linear octree AMR (Morton IDs, SoA fields, 2:1 balance) — see ADR-0002.
             geometry/  Stage-1 STL ingestion: read/clean, BVH classify (FLUID/CUT/INSIDE_SOLID),
                        feature detection, geometry-driven refinement, JSON report.
             flow/      Baseline incompressible NS solver (staggered MAC, RK3 projection,
                        WENO-5/central, CG pressure Poisson) on a uniform mesh — see ADR-0003.
             adaptive/  Physics-aware AMR + audited reduced-model classification on the octree:
                        sensors, AMR controller (static-box floors), classifier+acceptance gate,
                        hard guards, step-acceptance, audit trail, efficiency — see ADR-0004.
             io/        .vtu (ParaView) writer + HDF5 checkpoints.
             `nabla_solve ingest <stl> --case wall-mounted-cube` -> meshed .vtu + geometry JSON.
             `nabla_solve run <case.json> [--restart <ckpt>]`     -> diagnostics.jsonl + .vtu + checkpoints.
             `nabla_solve adapt --res N --re X`                   -> audit.jsonl + audit_summary.md + efficiency.txt + .vtu.
backend/     Python 3.12 FastAPI orchestration service. Managed with uv.
             /api/runs lifecycle (upload STL -> ingest -> start/pause/resume via solver
             checkpoints), artifact serving, WS telemetry, AI experiment co-pilot
             (Anthropic API; schema-gated — free text never reaches solver config).
             Storage + job-runner seams for Postgres/S3/queue later — see ADR-0005.
frontend/    React + TS + Vite SPA. Routes: /pre, /sim, /post.
validation/  Phase-0 validation harness (stdlib Python): reference registry (TODO slots — never
             fabricate values), postproc/ library, comparison engine (5-10% bands),
             `python -m validation.run_ladder` (Re 500-3000, checkpoint/resume) and
             `python -m validation.make_report` (investor-facing HTML report, --check).
docs/        Architecture Decision Records (ADRs).
scripts/     Dev tooling (dev.sh, build-core.sh).
```

## Common commands

```bash
# Everything, locally, one command:
scripts/dev.sh

# Core
scripts/build-core.sh                 # configure + build Release, prints --version
cmake -S core -B core/build -DNABLA_BUILD_TESTS=ON && cmake --build core/build -j
ctest --test-dir core/build --output-on-failure

# Backend (run inside backend/)
uv sync && uv run ruff check . && uv run mypy && uv run pytest
uv run uvicorn app.main:app --reload --ws wsproto --port 8000

# Frontend (run inside frontend/)
npm install && npm run lint && npm run typecheck
npm run dev                           # http://localhost:5173

# Validation
cd validation && python generate_report.py
```

## Conventions

- **Core (C++):** C++20, no exceptions across the IPC boundary (errors → non-zero
  exit + stderr). Warnings are on (`-Wall -Wextra -Wpedantic -Wconversion`); keep
  builds clean. The version is single-sourced from `project(VERSION ...)` in
  [`core/CMakeLists.txt`](core/CMakeLists.txt).
- **Backend (Python):** ruff + mypy `--strict` must pass. Fully typed. The
  backend contains **no physics** — only orchestration and validation.
- **Frontend (TS):** ESLint flat config + `tsc` must pass. No direct solver
  access; reach the backend via `/api/*` (proxied to `:8000` in dev).
- **ADRs:** one decision per file, immutable once Accepted; supersede rather than
  edit. See [docs/adr/](docs/adr/).

## Definition of done for a change

- Touched layer's quality gates pass (see Common commands).
- The three-layer and no-GPL rules are not violated.
- If the IPC contract changed, both the core and backend sides were updated
  together, and `validation/` still passes.
