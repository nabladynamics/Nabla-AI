# 0006. Containerized deployment: docker-compose as the on-prem story

- **Status:** Accepted
- **Date:** 2026-06-12
- **Deciders:** Founding engineering team

> Numbering note: the product spec refers to this decision as "ADR-004". ADR-0004
> was already taken by the adaptive layer, and ADRs are immutable once accepted —
> so this record is 0006.

## Context and Problem Statement

Phase 0 runs everything on one machine (ADR-0001). The next buyers we care
about — aerospace/defense — will not send geometry to anyone's cloud: they
expect the product delivered **onto their hardware**, often air-gapped. We
also want a hosted SaaS later without maintaining a second deployment
mechanism. How do we package the three layers so that local dev, investor
demos, on-prem delivery, and future SaaS are the *same* artifact?

## Decision Drivers

- One packaging mechanism for all four contexts; no deployment fork to maintain.
- The trade-secret boundary: the solver binary must never be distributed on
  its own or reachable except through the backend API (CLAUDE.md rule 4).
- Air-gap friendliness: no cloud account, no external service at runtime.
- Reproducibility: every shipped result must be traceable to an exact build.
- A clean machine with Docker must reach a working demo in one command.

## Considered Options

1. **docker-compose file shipping the 3-layer stack** (chosen).
2. Kubernetes/Helm from day 1 — operational overkill for a two-service stack;
   most on-prem clients have Docker, far fewer run clusters.
3. Native installers / systemd units — three toolchains (C++, Python, Node) on
   unknown client OSes; exactly the dependency matrix containers eliminate.

## Decision

**`docker-compose.yml` at the repo root *is* the deployment unit** — for local
demos today and for on-prem clients in the next phase. Hosted SaaS later runs
the same two images behind a managed proxy.

The stack is **two running services**, mirroring the architecture's process
boundaries (frontend → backend → core, ADR-0001):

- **`backend`** ([docker/backend.Dockerfile](../../docker/backend.Dockerfile)) —
  Python 3.12 + FastAPI, with the solver compiled in a build stage and baked
  into the image at `/usr/local/bin/nabla_solve`. The core is *not* a service:
  it is a CLI the backend invokes over file-based IPC, so it lives inside the
  backend container — behind the API, satisfying the trade-secret boundary.
  Run data lives on the named volume `nabla-data` (survives image upgrades).
  The validation harness is included, so report generation works offline.
- **`frontend`** ([docker/frontend.Dockerfile](../../docker/frontend.Dockerfile)) —
  static Vite build served by nginx, which also reverse-proxies `/api`
  (including the telemetry WebSocket) to `backend`. The browser only ever
  talks same-origin HTTP — identical to the Vite dev proxy.

Additionally, [docker/core.Dockerfile](../../docker/core.Dockerfile) builds a
**solver-only artifact image** (multi-stage, `debian:bookworm-slim` runtime,
stripped binary, non-root). It is *not* part of the running stack
(`profiles: ["tools"]` in compose) — it exists for CI and for internal
distribution of the solver between our own build systems. The core build
stage is duplicated between `core.Dockerfile` and `backend.Dockerfile`
(~10 lines, marked "keep in lockstep"): we chose that small duplication over
cross-image `COPY --from` so that a clean `docker compose up` needs no
pre-built images and no registry.

Supporting decisions:

- **Build provenance.** `.git` is excluded from the build context; the short
  git SHA enters as the `NABLA_GIT_SHA` build arg, is baked into the solver's
  generated `version.hpp`, and is stamped by the solver into every
  `diagnostics.jsonl` / `audit.jsonl` (leading `{"event":"meta",…}` line),
  every geometry report, and the validation report header. The backend
  reports its own SHA at `/health`.
- **Configuration via environment only** ([.env.example](../../.env.example)):
  host ports, data dir, solver command, API keys. Secrets are never committed
  (`.env` is git- and docker-ignored); the Anthropic key rides the standard
  `ANTHROPIC_API_KEY` variable and the stack runs fully featured without it
  (co-pilot disabled). Solver numerics are deliberately *not* env vars — they
  are per-run case configuration recorded with each run.
- **One-command demo.** [scripts/demo.sh](../../scripts/demo.sh) builds and
  starts the stack, waits for health, seeds a wall-mounted-cube run from the
  bundled STL, and opens the browser. CI runs the same path end-to-end
  (50-step miniature case → report generation) on every push.

## Consequences

- **Positive:** identical artifact from laptop to air-gapped site; the demo
  is a button; the solver binary never leaves the backend image; every result
  file names the build that produced it.
- **Negative / trade-offs:** the core build stage exists twice (lockstep
  comment, ~10 lines); compose offers no orchestration features (scaling,
  rolling upgrades) — acceptable for single-machine Phase 0 and revisited only
  when SaaS load demands it; images are rebuilt rather than patched.
- **Follow-ups:** image signing + SBOM for defense procurement; offline image
  bundles (`docker save`) for air-gapped installs; Postgres/S3 storage
  implementations (ADR-0005) become compose services when needed.
