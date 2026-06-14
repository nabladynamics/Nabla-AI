# ∇ Nabla AI

A **physics-adaptive CFD platform**: an octree-AMR incompressible Navier-Stokes
solver whose reduced-physics decisions are *proposed by sensors, accepted by an
NS-consistency gate, and fully audited* — wrapped in a local-first web product.

> Architecture rules live in [`CLAUDE.md`](CLAUDE.md); decisions and rationale
> in [docs/adr/](docs/adr/).

## Architecture 

Three layers, three processes, one rule: each layer talks only to its
immediate neighbor ([ADR-0001](docs/adr/0001-local-first-client-server-architecture.md)).

```
┌────────────────────────┐   HTTP + WebSocket    ┌───────────────────────────┐
│  frontend  (React/TS)  │ ────────────────────► │  backend  (FastAPI)       │
│  /pre  /sim  /post     │   /api/* + telemetry  │  runs, jobs, artifacts,   │
│  presentation only     │ ◄──────────────────── │  AI co-pilot (schema-gated)│
└────────────────────────┘                       └─────────────┬─────────────┘
                                                               │ file-based IPC
                                                  spec/case.json ▼  diagnostics,
                                                 ┌───────────────────────────┐
                                                 │  core  (C++20, headless)  │
                                                 │  nabla_solve: octree AMR, │
                                                 │  MAC/RK3/WENO-5 NS,       │
                                                 │  audited adaptive physics │
                                                 └───────────────────────────┘
```

- The backend is the **only** layer that invokes the solver (trade-secret
  boundary — the binary is never distributed on its own).
- Core ↔ backend coupling is **only** plain files: a case/spec file in, JSONL
  diagnostics + VTU/HDF5 artifacts out.
- The core depends on permissively licensed code only — **no GPL, ever**.
- Every result file carries the solver version + git SHA that produced it
  (leading `meta` line in `diagnostics.jsonl` / `audit.jsonl`, fields in the
  geometry report, header of the validation report).

| Path          | Stack                          | Role                                                   |
| ------------- | ------------------------------ | ------------------------------------------------------ |
| [`core/`](core/)             | C++20, CMake ≥ 3.25 | Headless solver. `nabla_core`, `nabla_solve`, `nabla_tests`. |
| [`backend/`](backend/)       | Python 3.12, FastAPI, uv | Orchestration. The only layer that invokes the core.   |
| [`frontend/`](frontend/)     | React + TS + Vite   | SPA. Routes `/pre`, `/sim`, `/post`.                   |
| [`validation/`](validation/) | Python (stdlib)     | Reference registry, comparison engine, report generator. |
| [`docs/`](docs/)             | Markdown            | Architecture Decision Records.                         |
| [`scripts/`](scripts/)       | Bash                | `dev.sh`, `demo.sh`, `build-core.sh`.                  |
| [`docker/`](docker/)         | Dockerfiles + nginx | Container images ([ADR-0006](docs/adr/0006-containerized-deployment.md)). |

## Quickstart

### Demo in one command (Docker only)

```bash
scripts/demo.sh
```

Builds the images, starts the stack, seeds a wall-mounted-cube run at
Re_h = 500 from the bundled STL, and opens the browser. `scripts/demo.sh down`
stops it. This same compose file is the on-prem deployment unit
([ADR-0006](docs/adr/0006-containerized-deployment.md)):

```bash
docker compose up            # web → :8080, api → :8000
```

### Local development (native toolchains)

Requires a C++20 compiler, CMake ≥ 3.25, [uv](https://docs.astral.sh/uv/), and
Node.js ≥ 18.

```bash
scripts/dev.sh
# backend  → http://localhost:8000/docs
# frontend → http://localhost:5173
```

Build just the core and check its provenance stamp:

```bash
scripts/build-core.sh
core/build/nabla_solve --version    # -> nabla_solve 0.1.0 (git <sha>)
```

### Configuration

All tunables are environment variables with safe defaults — see
[`.env.example`](.env.example) (`cp .env.example .env`, never commit `.env`).
The Anthropic API key (optional, enables the AI co-pilot) rides the standard
`ANTHROPIC_API_KEY` variable. Solver numerics are *not* env vars: they are
per-run case configuration, recorded with each run.

## Phase-0 run book

The investor-facing validation flow, end to end. (Through the UI: Pre → launch
→ Sim → Post → Exports. Below is the headless equivalent.)

**1. Ingest geometry** — STL → cleaned mesh, classification, geometry report:

```bash
core/build/nabla_solve ingest core/examples/cube.stl --case wall-mounted-cube \
  --out-vtu /tmp/cube.vtu --report /tmp/cube.geometry.json
```

**2. Run the validation ladder** — Re 500 → 3000, uniform + adaptive at every
rung, checkpoint/resume safe:

```bash
python3 -m validation.run_ladder --solver core/build/nabla_solve --out validation/runs
```

**3. Generate the report** — experiment card, force histories, spectra +
Strouhal, recirculation maps, audit table, speedup, reference comparison:

```bash
python3 -m validation.make_report --rung validation/runs/re0500 --out report.html
```

The report header names the exact solver build (version + git SHA) that
produced the data. Reference slots without digitized literature values render
as TODO — the harness **never fabricates reference values**.

Through the product instead: `POST /api/runs` (STL + config) → `/start` →
live telemetry on `/sim` → `/post` Exports tab → *Generate report*.

## Deployment (hosted demo)

Two targets. `docker compose up` (above) remains the local/on-prem path and is
unchanged; the same backend code runs in all three.

**Backend + solver → Railway** (one container; the solver runs as a backend
subprocess and streams telemetry over WebSocket, so they must share a machine —
and the solver core is never exposed on its own, per
[ADR-0001](docs/adr/0001-local-first-client-server-architecture.md)). The
backend cannot run on Vercel: Vercel is serverless with short request timeouts,
while a CFD run is minutes of CPU in a long-running subprocess.

- Dockerfile: [`Dockerfile.railway`](Dockerfile.railway) (config in
  [`railway.json`](railway.json) — `/health` healthcheck, restart-on-failure).
- Railway injects `PORT`; uvicorn binds `0.0.0.0:$PORT` (never hardcoded).
- HTTP requests return immediately (jobs run in the background); nothing a
  request-timeout could kill. Size the service for multi-minute CPU runs.
- Set `NABLA_ALLOWED_ORIGINS` to the Vercel domain (defaults to `*` for the
  demo). The Anthropic co-pilot key, if used, rides `ANTHROPIC_API_KEY`.
- Run data (`/data`: SQLite + artifacts) is container-local and **ephemeral** —
  wiped on redeploy. Attach a Railway volume at `/data` if runs must persist
  (no code change; the path is env-driven).

**Frontend → Vercel** (standalone static SPA):

- **Root Directory = `frontend`** (not the repo root); framework preset **Vite**.
- Set **`VITE_API_BASE_URL`** to the Railway backend URL (e.g.
  `https://nabla-backend.up.railway.app`). Every REST call and the telemetry
  WebSocket derive from it ([`frontend/src/config.ts`](frontend/src/config.ts));
  unset = same-origin for local dev. SPA deep-links (`/pre`, `/sim`, `/post`)
  are handled by [`frontend/vercel.json`](frontend/vercel.json).

## CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml):

- **core** — build + 47 Catch2 tests + flow validations (Ghia cavity,
  Poiseuille, ≤ 2% L2) + ingest/adapt smoke tests.
- **backend** — ruff, mypy `--strict`, pytest (lifecycle against a stub solver).
- **frontend** — ESLint + `tsc`.
- **validation harness** — stdlib unit tests.
- **docker smoke** — builds all three images, runs a 50-step miniature cube
  case end-to-end through the API in the composed stack, and asserts the
  validation report generates.
