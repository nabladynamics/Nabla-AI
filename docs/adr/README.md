# Architecture Decision Records

An ADR captures a single architecturally significant decision: its context, the
decision itself, and its consequences. We use a lightweight
[MADR](https://adr.github.io/madr/)-style format — see [`template.md`](template.md).

## Index

| ADR                                                          | Status   | Title                                                  |
| ------------------------------------------------------------ | -------- | ------------------------------------------------------ |
| [0001](0001-local-first-client-server-architecture.md)       | Accepted | Local-first client–server architecture for Phase 0     |
| [0002](0002-octree-from-scratch.md)                          | Accepted | Implement the adaptive octree from scratch             |
| [0003](0003-baseline-ns-solver.md)                           | Accepted | Baseline incompressible Navier–Stokes solver           |
| [0004](0004-adaptive-layer.md)                               | Accepted | Physics-aware adaptive layer with an audited trail     |
| [0005](0005-backend-orchestration-and-storage.md)            | Accepted | Backend orchestration: storage, job seam, AI gateway   |

## Conventions

- Files are named `NNNN-kebab-case-title.md` with a zero-padded sequence number.
- ADRs are immutable once **Accepted**. To change a decision, add a new ADR that
  supersedes the old one and update the old one's status to **Superseded**.
