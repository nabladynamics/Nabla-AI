"""Post-processing library for Nabla AI solver output.

Pure standard library. Reads the solver's run artifacts:

- ``diagnostics.jsonl``        per-step forces/residuals (uniform NS run)
- ``snapshot_*.vtu``           uniform-grid field snapshots
- ``audit.jsonl``              adaptive-layer decision trail
- ``adaptive_final.vtu``       octree cells with level / physics_mode labels
"""

from validation.postproc.diagnostics import load_diagnostics, force_stats
from validation.postproc.meanfield import mean_fields, select_window_snapshots
from validation.postproc.recirculation import recirculation_metrics
from validation.postproc.spectra import strouhal_analysis
from validation.postproc.vtu import load_octree_vtu, load_uniform_vtu

__all__ = [
    "load_diagnostics",
    "force_stats",
    "mean_fields",
    "select_window_snapshots",
    "recirculation_metrics",
    "strouhal_analysis",
    "load_octree_vtu",
    "load_uniform_vtu",
]
