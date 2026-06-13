# 0005. Backend orchestration: storage abstraction, job seam, AI gateway

- **Status:** Accepted
- **Date:** 2026-06-11
- **Deciders:** Founding engineering team

> Numbering note: the product spec refers to this decision as "ADR-003". ADR-0003
> was already taken by the baseline NS solver, and ADRs are immutable once
> accepted — so this record is 0005.

## Context and Problem Statement

The backend wraps the solver core: it is the **only** component that ever
touches the solver binary (ADR-0001's trade-secret boundary — the frontend
never does). Phase 0 runs on one machine, but the same service must later run
as hosted SaaS (Postgres/Supabase + S3, a distributed job queue) without
rewriting call sites. Three seams need deciding now.

## Decisions

### 1. Storage abstraction from day 1

Two narrow interfaces in `app/storage/base.py`:

- **`RunStore`** — run metadata CRUD (id, case config JSON, status
  created/meshing/running/paused/completed/failed, timestamps, artifact paths).
  Phase-0 implementation: **SQLite** (stdlib, WAL, thread-safe). Postgres /
  Supabase implements the same five methods.
- **`ArtifactStore`** — binary artifacts (STL, meshes, snapshots, checkpoints,
  reports) keyed by `(run_id, relative name)`. Phase-0 implementation: **local
  filesystem** rooted at `NABLA_DATA_DIR/artifacts/<run_id>/`. S3 implements
  the same interface.

One deliberate wrinkle: the solver communicates via **files** (file-based IPC,
ADR-0001), so the interface exposes `workspace(run_id) -> Path` — a real local
directory the solver process reads/writes. The filesystem store returns the
artifact directory itself; a remote store satisfies the contract by staging a
local scratch directory and syncing after each job. Call sites depend on
nothing beyond the interface, which is enforced by **contract tests** that run
the same behavioral suite against every implementation.

### 2. Job execution seam

`app/jobs.py` defines `JobRunner` (submit / cancel / is_active / shutdown).
Phase 0 ships `LocalJobRunner`: a FIFO queue and a single worker thread that
manages the solver as a subprocess (stdout/stderr to a per-run `solver.log`,
SIGTERM on cancel). Pause/resume builds on the solver's own checkpoints:
pause terminates the process; resume finds the latest `checkpoint_N.ckpt` and
restarts with `restart_from` + the remaining step budget. A distributed queue
(Celery, SQS, ...) replaces `LocalJobRunner` behind the same interface.

### 3. AI gateway: models propose, schemas accept

`POST /api/runs/{id}/ai/ask` sends the geometry report + conversation to the
Anthropic API with a system prompt that makes Claude the experiment-definition
co-pilot (identify the object; ask the disambiguating questions — experiment
type, inlet velocity/Re, target outputs, steady vs transient; then emit an
experiment card). Three defense layers keep free text away from the solver:

1. The card is emitted via **strict tool use** — the API constrains the JSON
   to the schema.
2. The backend re-validates with Pydantic (`extra="forbid"`, bounded fields);
   a non-conforming card is a 502, never a config.
3. The card maps onto `CaseConfig` through a **typed whitelist**
   (`card_to_case_config`): enum case type, bounded numerics, name reduced to
   a slug. `CaseConfig` itself is the only shape the solver ever receives.

## Consequences

- **Positive:** swap-ready persistence and job execution; the trade-secret
  boundary holds (solver binary behind the backend only); telemetry (WS tail of
  `diagnostics.jsonl` + `audit.jsonl`) and artifact serving are storage-agnostic;
  the AI layer cannot inject anything unvalidated into a run.
- **Trade-offs:** single worker = one solver job at a time (fine for Phase 0);
  WS telemetry polls files (~4 Hz) rather than using inotify — simple and
  portable, revisit if run counts grow; SQLite is process-local by design.
- **Follow-ups:** Postgres + S3 implementations; multi-worker queue; auth at
  the backend boundary before any non-local deployment.
