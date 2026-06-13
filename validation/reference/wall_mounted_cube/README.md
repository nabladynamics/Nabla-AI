# Reference-data registry — wall-mounted cube in channel flow

Digitized reference results for the Phase-0 validation ladder
(Re_h = 500, 1000, 1700, 3000). One folder per rung (`re0500/` … `re3000/`),
each holding:

| File | Contents |
| ---- | -------- |
| `metrics.csv` | Scalar metrics: separation/reattachment lengths, Cd, Cl, Strouhal, Cp, reverse-flow volume. |
| `profiles/u_profile_xrel_*.csv` | Mean streamwise-velocity profiles u(y)/U on the spanwise centerplane at standard stations. |

Stations are measured **relative to the cube front face** in cube heights:
x/h ∈ {−1.0, 0.5, 1.5, 2.5, 4.0} (0.5 is over the cube roof).

## Status: TODO — values must be digitized, never fabricated

Every `value` field currently reads `TODO`. The comparison engine treats those
rows as **NO-REF (TODO)** and will not pass or fail against them. **Do not
insert numbers from memory or estimates** — only values digitized from the
canonical literature, with the source recorded in `sources.csv`.

## Digitization protocol

1. Pick a source from `sources.csv` (or add one — full citation required).
2. Verify the regime actually matches the rung: same Re_h definition
   (U_bulk · h / ν), channel confinement (here height = 3h), cube on the floor,
   developed inflow vs uniform inflow (we use **uniform inflow** in Phase 0 —
   note the difference in `notes` when sources use developed channel inflow).
3. Digitize plots (e.g. WebPlotDigitizer); record the figure number and axis
   calibration in the `notes` column. Tabulated values are preferred.
4. Fill `value` (+ `uncertainty` when the source provides error bars), set
   `source_id`, and flip the `status` in `sources.csv` to `digitized`.

## Candidate sources (`sources.csv`)

The registry seeds candidate canonical sources covering the laminar→transitional
range of the ladder and the methodology references used at higher Re. Each
entry is marked `candidate-verify`: **verify the citation and regime before
digitizing from it.** Known regime caveats are recorded per source.
