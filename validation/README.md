# nabla validation

Reference data, a comparison script, and a report generator for the solver
core. Pure Python 3.12 standard library — no third-party dependencies.

## Layout

| Path                       | Role                                                |
| -------------------------- | --------------------------------------------------- |
| `cases/*.spec`             | Solver input specs for canonical cases.             |
| `reference/*.expected`     | Reference results + absolute tolerances per case.   |
| `compare.py`               | Compare one result file against one reference file. |
| `generate_report.py`       | Run every case via the solver and write `report.md`.|

A case is `cases/<name>.spec` paired with `reference/<name>.expected`.

## Run

Build the core first (`scripts/build-core.sh`), then:

```bash
cd validation
python generate_report.py          # writes report.md
# or compare a single result you produced yourself:
python compare.py --result /tmp/case.result --expected reference/diffusion_box.expected
```

`generate_report.py` exits non-zero if any check fails, so it can gate a
release. With `--strict` it also fails when the solver binary is missing.

## Flow validations (baseline Navier–Stokes solver)

[`flow/`](flow/) holds the quantitative CFD validations for the baseline NS
solver, each with a **≤2% relative-L2** pass threshold:

| Case | Reference | File |
| ---- | --------- | ---- |
| Lid-driven cavity, Re=100 | Ghia, Ghia & Shin (1982) centerline | `flow/cavity_re100.json`, `flow/ghia_re100_u.csv` |
| Poiseuille channel | analytic parabola | `flow/channel_poiseuille.json` |

```bash
python flow/validate_flow.py --solver ../core/build/nabla_solve
# [PASS] Poiseuille (analytic): L2 = 0.026%  (threshold 2%)
# [PASS] Lid cavity Re=100 (Ghia): L2 = 0.453%  (threshold 2%)
```

`validate_flow.py` runs each case through `nabla_solve run`, extracts the
centerline/outlet profile from the produced `.vtu`, compares to the reference,
and exits non-zero if any case exceeds the threshold. It runs in CI.

## Phase-0 validation harness (wall-mounted cube)

The investor-facing harness: reference registry + post-processing library +
comparison engine + report generator + experiment runner. Stdlib Python only.

| Path | Role |
| ---- | ---- |
| [`reference/wall_mounted_cube/`](reference/wall_mounted_cube/) | Reference registry for Re_h = 500–3000. **All values are TODO slots** — digitize from the cited literature, never fabricate (see its README). |
| [`postproc/`](postproc/) | Library: VTU/diagnostics/audit readers, mean fields, floor critical points, recirculation metrics, force stats, DFT → Strouhal, Q-criterion OBJ export, SVG figures. |
| `postproc/compare_ref.py` | Comparison engine with the spec's acceptance bands (St/Cd/Cl/Cp: 5–10%; lengths: documented 10–20% default). |
| `make_report.py` | One command → self-contained HTML report (print-to-PDF), every section from real solver output. `--check` fails on any empty section. |
| `run_ladder.py` | Re ladder 500 → 3000, sequential, **checkpoint/resume** (re-running is always safe). |
| `tests/` | Unit tests (`python3 -m unittest discover -s validation/tests -t .`, in CI). |

```bash
# from the repo root, with the core built:
python3 -m validation.run_ladder --only-re 500          # run (or resume) the Re=500 rung
python3 -m validation.make_report --rung validation/runs/re0500 --check
open validation/runs/re0500/report.html                 # print to PDF from the browser
```
