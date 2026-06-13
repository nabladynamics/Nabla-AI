# 0004. Physics-aware adaptive layer with an audited decision trail

- **Status:** Accepted
- **Date:** 2026-06-11
- **Deciders:** Founding engineering team

## Context and Problem Statement

The adaptive layer is Nabla AI's core differentiator: known physics where
defensible, full NS/DNS-oriented refinement where the flow demands it, with
**every decision audited**. It sits on top of the validated baseline solver
(ADR-0003) and must provide per-cell physical sensors, a physics-aware AMR
controller, a (swappable) reduced-model classifier whose proposals are only
*accepted* by NS-consistency checks, hard guards that force full physics in
known-critical zones, a step-acceptance controller, an efficiency metric, and a
first-class audit trail.

## Decisions

### Control plane on the octree; flow behind a `SolverBackend` seam
The entire control plane — `PhysicalSensorEvaluator`, `OctreeAMRController`,
`PhysicsModeClassifier` + acceptance gate, `GuardZones`, `StepAcceptanceController`,
`AuditTrail`, `EfficiencyMonitor` — operates on the **octree mesh** and is
independent of how the flow is advanced. The advance is hidden behind
`SolverBackend`.

**The shipped backend wraps the validated baseline** (the Prompt-4 staggered-MAC
solver): it advances the flow and the field is sampled onto the octree, which the
control plane refines/coarsens, classifies, guards, and audits. Cd/Cl are
therefore the **validated** values, not the output of a new, unvalidated solver.
The octree is the adaptive **resolution + model map**; the efficiency metric
reports its cell count against a uniform-fine octree (whole domain at the finest
level). This is the honest MVP: nothing shipped is unvalidated.

An **octree-native FV NS solve** (generalizing the baseline kernels onto the
graded octree with hanging-node fluxes and a checkerboard-free projection) is the
next increment and drops in behind `SolverBackend` without touching the control
plane. A genuine **conservative octree transport solve** is already implemented
(`octreeTransportStep`) and is exercised by the live-solve conservation test, so
the octree carries a real, conservative PDE advance today.

### Rule-based classifier, ML-ready
`PhysicsModeClassifier` is an interface; `RuleBasedClassifier` is the MVP. An ML
model implements the same `propose()` and slots in unchanged. Crucially, a
reduced model may only **propose**; `acceptReducedModel()` (NS-consistency:
reverse-flow / y+ validity / vorticity / strain checks) is the only thing that
**accepts**. On rejection the cell is promoted to FULL_NS and the reason is
logged. Example: a log-law (NEAR_WALL) proposal is rejected wherever reverse flow
exists.

### Hard guards are geometric and unconditional
`GuardZones` force FULL_NS in the horseshoe-vortex birth region, the
cube-front/floor junction, the top/side leading edges, and the near-wake start.
They depend only on geometry and are applied *after* the classifier, so **no
configuration of thresholds can override them** (enforced by test).

### Static boxes A–E are minimum-resolution floors
The Phase-0 spec's boxes (scaled by cube height) are enforced as minimum levels:
the controller refines any cell below its box floor and never coarsens below it.

### Step acceptance and remedy order
A step is accepted only when spatial, temporal, and convergence criteria pass; on
rejection the remedy is chosen in the spec's priority order: **refine mesh →
reduce Δt → promote models**.

### Audit trail is a product feature
`AuditTrail` writes one JSON object per event to `audit.jsonl` — step
begin/end, refinements/coarsenings (with reasons), per-cell model-label changes
(with reasons), and per-region reduced-model accept/reject — plus a Markdown
summary table. It is treated as a first-class output, not a debug log.

## Consequences

- **Positive:** the full audited control plane is real and unit-tested
  (conservation under live refine/coarsen; classifier rejects log-law under
  reverse flow; guards non-overridable; static-box floors). Cd/Cl are the
  validated baseline values; the adaptive octree demonstrates a materially lower
  cell count vs uniform-fine, with every decision explained in the audit.
- **Trade-offs / honest scope:** flow is advanced by the baseline (not yet an
  octree-native NS solve), so the demonstrated saving is in **cells** (and the
  control-plane cost), with full compute savings arriving when the octree-native
  backend lands behind the seam. Sensors driving the demo are sampled from the
  baseline field. Sensor formulas use standard, documented proxies (e.g., a
  Richardson-style neighbour-deviation truncation estimate).
- **Follow-ups:** octree-native NS backend; ML classifier behind the same
  interface; richer per-region monitors (Cd/Cl contribution per box) in the audit.
