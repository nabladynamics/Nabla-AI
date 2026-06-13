"""Floor wall-shear analysis and critical-point extraction.

The wall shear on the floor (y=0, no-slip) is estimated from the first cell row:
tau_x ~ nu * u(i, 0, k) / (dy/2). Separation / attachment points are the sign
changes of tau_x along a line: + -> -  separation, - -> +  attachment.
"""

from __future__ import annotations

from dataclasses import dataclass

from validation.postproc.vtu import UniformGrid


@dataclass(frozen=True)
class CriticalPoint:
    x: float
    kind: str  # "separation" (+ -> -) or "attachment" (- -> +)


def floor_tau_x(
    grid: UniformGrid, u: list[float], solid: list[float], nu: float, k_row: int
) -> list[tuple[float, float]]:
    """(x, tau_x) along the floor row j=0 at spanwise index k_row.

    Solid cells (inside the cube footprint) are skipped — there is no floor
    shear under the body.
    """
    out = []
    for i in range(grid.nx):
        c = grid.cidx(i, 0, k_row)
        if solid[c] > 0.5:
            continue
        x = grid.center(i, 0, k_row)[0]
        tau = nu * u[c] / (grid.dy / 2.0)
        out.append((x, tau))
    return out


def critical_points(tau_line: list[tuple[float, float]]) -> list[CriticalPoint]:
    """Sign-change crossings of tau_x with linear interpolation of x."""
    pts: list[CriticalPoint] = []
    for (x1, t1), (x2, t2) in zip(tau_line, tau_line[1:]):
        if x2 - x1 > 3.0 * (tau_line[1][0] - tau_line[0][0]):
            continue  # gap across the cube footprint — not a real crossing
        if t1 == 0.0 or t1 * t2 >= 0.0:
            continue
        xc = x1 - t1 * (x2 - x1) / (t2 - t1)
        pts.append(CriticalPoint(xc, "separation" if t1 > 0.0 else "attachment"))
    return pts


def nearest_k(grid: UniformGrid, z: float) -> int:
    """Spanwise cell row whose center is closest to z."""
    best, best_d = 0, float("inf")
    for k in range(grid.nz):
        d = abs(grid.center(0, 0, k)[2] - z)
        if d < best_d:
            best, best_d = k, d
    return best
