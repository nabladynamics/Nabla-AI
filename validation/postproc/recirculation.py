"""Recirculation metrics for the wall-mounted cube.

All lengths are normalized by the cube height h. Conventions:
- upstream separation distance  = x_front - x_S   (horseshoe separation; the
  most upstream + -> - crossing within 3h ahead of the front face)
- reattachment length L_R       = x_R - x_rear    (first - -> + crossing behind
  the rear face on the floor centerline)
- reverse-flow volume           = sum of fluid cell volumes with u_mean < 0, /h^3
"""

from __future__ import annotations

from dataclasses import dataclass, field

from validation.postproc.vtu import UniformGrid
from validation.postproc.walls import CriticalPoint, critical_points, floor_tau_x, nearest_k


@dataclass
class RecirculationMetrics:
    x_separation_upstream: float | None = None     # absolute x of horseshoe separation
    upstream_separation_over_h: float | None = None
    x_reattachment: float | None = None            # absolute x on the floor centerline
    reattachment_over_h: float | None = None
    rear_bubble_over_h: float | None = None        # == reattachment length (floor centerline)
    reverse_flow_volume_over_h3: float = 0.0
    critical_pts: list[CriticalPoint] = field(default_factory=list)


def recirculation_metrics(
    grid: UniformGrid,
    u: list[float],
    solid: list[float],
    cube_bbox: tuple[float, float, float, float, float, float],
    nu: float,
    h: float,
) -> RecirculationMetrics:
    x_front, x_rear = cube_bbox[0], cube_bbox[1]
    z_center = 0.5 * (cube_bbox[4] + cube_bbox[5])
    k_row = nearest_k(grid, z_center)

    m = RecirculationMetrics()
    tau = floor_tau_x(grid, u, solid, nu, k_row)
    m.critical_pts = critical_points(tau)

    # Horseshoe separation: most upstream + -> - crossing in [x_front-3h, x_front).
    sep = [p.x for p in m.critical_pts if p.kind == "separation" and x_front - 3 * h <= p.x < x_front]
    if sep:
        m.x_separation_upstream = min(sep)
        m.upstream_separation_over_h = (x_front - m.x_separation_upstream) / h

    # Reattachment: first - -> + crossing behind the rear face.
    att = [p.x for p in m.critical_pts if p.kind == "attachment" and p.x > x_rear]
    if att:
        m.x_reattachment = min(att)
        m.reattachment_over_h = (m.x_reattachment - x_rear) / h
        m.rear_bubble_over_h = m.reattachment_over_h

    vol = 0.0
    cell_vol = grid.cell_volume()
    for c in range(grid.cells):
        if solid[c] < 0.5 and u[c] < 0.0:
            vol += cell_vol
    m.reverse_flow_volume_over_h3 = vol / h**3
    return m
