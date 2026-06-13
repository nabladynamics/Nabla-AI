# 0001. Local-first client–server architecture for Phase 0

- **Status:** Accepted
- **Date:** 2026-06-11
- **Deciders:** Founding engineering team

## Context and Problem Statement

Nabla AI is a physics-adaptive CFD platform. The solver core is the company's
primary intellectual property and long-term competitive advantage. We need an
architecture for **Phase 0** (initial product) that:

1. Lets a single developer or early user run the *entire* product on one
   machine, with no cloud account or network dependency.
2. Does **not** paint us into a corner — the same codebase must grow into a
   hosted, multi-tenant SaaS and an on-prem enterprise offering without a
   rewrite.
3. Protects the solver core as a trade secret. It must never be handed to a
   client as a raw, reverse-engineerable binary or as source.

A naive desktop app (one monolith shipped to the user, solver linked into the
client) optimizes (1) but fails (2) and (3): the solver ends up on the client
machine, and the architecture does not resemble the eventual cloud product.

## Decision Drivers

- **Trade-secret protection.** The solver is the moat; it cannot ship to
  clients.
- **Single deployment topology** across local dev, hosted SaaS, and on-prem, to
  avoid divergent code paths.
- **Phase-0 simplicity** — must run end to end on a laptop with one command.
- **Clear layer boundaries** so the team can work on UI, orchestration, and
  numerics independently.

## Considered Options

1. **Monolithic desktop application** — React/Electron UI with the solver linked
   in-process. Simple, fully offline, but ships the solver to clients and shares
   nothing with a future cloud product.
2. **Cloud-only SaaS from day one** — no local story. Maximizes IP protection
   but blocks fast local iteration and an on-prem path, and is heavy for Phase 0.
3. **Cloud-native client–server architecture, executed locally** *(chosen)* —
   browser/SPA frontend ↔ backend orchestration service ↔ solver core, all three
   running as local processes in Phase 0, but wired exactly as they would be in
   the cloud.

## Decision

We adopt a **cloud-native client–server architecture that is executed locally**
for Phase 0.

- Three layers, same boundaries everywhere we deploy:
  - **Frontend** (React/TS SPA): presentation only. Talks to the backend over
    HTTP; never to the solver.
  - **Backend** (Python/FastAPI): orchestration, validation, job management. The
    *only* component that invokes the solver.
  - **Core** (C++ solver): headless numerics. No UI, no network server.
- **The solver core never ships to clients as a raw binary.** It lives behind
  the backend. Clients send a problem specification and receive results; they
  never receive the executable or its source. This is the trade-secret boundary.
- **File-based IPC** between backend and core: the backend writes a spec file,
  invokes `nabla_solve`, and reads a result file. This keeps the core a
  self-contained process with a tiny, stable, language-agnostic contract — easy
  to sandbox, containerize, or move behind a queue later.
- **Phase 0 runs entirely locally.** `scripts/dev.sh` builds the core and starts
  the backend (`:8000`) and frontend (`:5173`) on one machine. The same three
  processes, unchanged, become containers in the cloud.

## Consequences

- **Positive**
  - The solver stays server-side in every topology; clients never hold the
    binary. IP is protected by construction, not by obfuscation.
  - One architecture spans local dev → hosted SaaS → on-prem. The local setup is
    a deployment of the production design, not a throwaway prototype.
  - **On-prem path is natural:** because the layers are already separate
    processes communicating over HTTP + files, an enterprise on-prem deployment
    is delivered as **containerized services** (frontend, backend, core image)
    the customer runs inside their own network — no architectural change, and the
    solver image can be access-controlled.
  - Layer boundaries let UI, orchestration, and numerics evolve independently.

- **Negative / trade-offs**
  - More moving parts than a monolith: three processes and an IPC contract to
    run even on a laptop. Mitigated by the single `scripts/dev.sh` entry point.
  - File-based IPC is simple but not the lowest-latency option; acceptable for
    Phase-0 batch solves and revisitable behind the same backend boundary.

- **Follow-ups**
  - Containerize each layer (Dockerfiles + compose) for the hosted and on-prem
    deployments.
  - Replace synchronous subprocess invocation with a job queue when solves grow
    long, keeping the file-based contract.
  - Define the authn/z model at the backend (the trust boundary) before any
    non-local deployment.
