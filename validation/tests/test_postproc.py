"""Unit tests for the validation post-processing library (stdlib unittest).

Run from the repo root:  python3 -m unittest discover -s validation/tests -t .
"""

from __future__ import annotations

import math
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from validation.postproc import audit as audit_mod
from validation.postproc import compare_ref, svg
from validation.postproc.recirculation import recirculation_metrics
from validation.postproc.spectra import strouhal_analysis
from validation.postproc.vtu import UniformGrid, load_uniform_vtu, solid_bbox


class TestSpectra(unittest.TestCase):
    def test_strouhal_peak_recovered_from_uneven_samples(self) -> None:
        f0 = 0.12
        t, cl = [], []
        ti = 0.0
        for i in range(512):
            ti += 0.05 * (1.0 + 0.1 * math.sin(i))  # CFL-like uneven dt
            t.append(ti)
            cl.append(0.3 + 0.2 * math.sin(2.0 * math.pi * f0 * ti))
        res = strouhal_analysis(t, cl, h=1.0, velocity=1.0)
        self.assertTrue(res.prominent)
        assert res.strouhal is not None
        self.assertLess(abs(res.strouhal - f0), 2.0 * res.resolution_st)

    def test_steady_signal_reports_no_peak(self) -> None:
        t = [0.05 * i for i in range(256)]
        cl = [0.5 + 1e-9 * i for i in range(256)]  # flat (steady wake)
        res = strouhal_analysis(t, cl)
        self.assertFalse(res.prominent)
        self.assertIsNone(res.strouhal)


class TestRecirculation(unittest.TestCase):
    def _grid(self) -> UniformGrid:
        # h = 1 with 4 cells per h; domain 10 x 1.5 x 2.75
        return UniformGrid(40, 6, 11, 0.25, 0.25, 0.25, 0.0, 0.0, 0.0)

    def test_metrics_on_synthetic_bubble(self) -> None:
        g = self._grid()
        u = [1.0] * g.cells
        solid = [0.0] * g.cells
        # cube footprint x in [3,4], y in [0,1], z in [1.25, 2.0 ): centered-ish
        for k in range(g.nz):
            for j in range(g.ny):
                for i in range(g.nx):
                    x, y, z = g.center(i, j, k)
                    if 3.0 <= x <= 4.0 and y <= 1.0 and 1.25 <= z <= 2.0:
                        solid[g.cidx(i, j, k)] = 1.0
        cube = solid_bbox(g, solid)
        assert cube is not None
        z_mid = 0.5 * (cube[4] + cube[5])
        # reverse flow on the floor: upstream pocket [2.5,3.0) and rear bubble [4,5.5)
        for k in range(g.nz):
            for i in range(g.nx):
                x, _, z = g.center(i, 0, k)
                if abs(z - z_mid) < 0.4 and (2.5 <= x < 3.0 or 4.0 <= x < 5.5):
                    c = g.cidx(i, 0, k)
                    if solid[c] < 0.5:
                        u[c] = -0.2
        m = recirculation_metrics(g, u, solid, cube, nu=1e-3, h=1.0)
        assert m.upstream_separation_over_h is not None
        assert m.reattachment_over_h is not None
        self.assertAlmostEqual(m.upstream_separation_over_h, 0.5, delta=0.3)
        self.assertAlmostEqual(m.reattachment_over_h, 1.5, delta=0.3)
        self.assertGreater(m.reverse_flow_volume_over_h3, 0.0)


class TestCompare(unittest.TestCase):
    def _ref(self, value: float | None) -> dict[str, compare_ref.RefEntry]:
        return {"cd_mean": compare_ref.RefEntry("cd_mean", value, "-", "SRC1", "")}

    def test_bands(self) -> None:
        for computed, expected in ((1.04, "PASS"), (1.08, "MARGINAL"), (1.25, "FAIL")):
            rows = compare_ref.compare({"cd_mean": computed}, self._ref(1.0))
            self.assertEqual(rows[0].status, expected, msg=f"computed={computed}")

    def test_todo_reference_never_passes_or_fails(self) -> None:
        rows = compare_ref.compare({"cd_mean": 1.0}, self._ref(None))
        self.assertEqual(rows[0].status, compare_ref.STATUS_TODO)

    def test_uncomputable_metric_is_na(self) -> None:
        rows = compare_ref.compare({"cd_mean": None}, self._ref(1.0))
        self.assertEqual(rows[0].status, compare_ref.STATUS_NA)

    def test_todo_csv_parses_as_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "metrics.csv"
            p.write_text("metric,value,uncertainty,units,source_id,notes\ncd_mean,TODO,,-,X,\n")
            ref = compare_ref.load_reference_metrics(p)
            self.assertIsNone(ref["cd_mean"].value)


class TestVtu(unittest.TestCase):
    def test_uniform_vtu_roundtrip_canonical_order(self) -> None:
        # 2x1x1 grid written deliberately in reversed cell order.
        def cell_pts(x0: float) -> str:
            x1 = x0 + 1.0
            return (
                f"{x0} 0 0 {x1} 0 0 {x1} 1 0 {x0} 1 0 "
                f"{x0} 0 1 {x1} 0 1 {x1} 1 1 {x0} 1 1"
            )

        xml = f"""<?xml version="1.0"?>
<VTKFile type="UnstructuredGrid" version="1.0" byte_order="LittleEndian" header_type="UInt64">
  <UnstructuredGrid><Piece NumberOfPoints="16" NumberOfCells="2">
    <Points><DataArray type="Float64" NumberOfComponents="3" format="ascii">
    {cell_pts(1.0)} {cell_pts(0.0)}
    </DataArray></Points>
    <Cells>
      <DataArray type="Int64" Name="connectivity" format="ascii">0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15</DataArray>
      <DataArray type="Int64" Name="offsets" format="ascii">8 16</DataArray>
      <DataArray type="UInt8" Name="types" format="ascii">12 12</DataArray>
    </Cells>
    <CellData Scalars="u"><DataArray type="Float64" Name="u" format="ascii">7.0 3.0</DataArray></CellData>
  </Piece></UnstructuredGrid></VTKFile>"""
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "t.vtu"
            p.write_text(xml)
            grid, fields = load_uniform_vtu(p)
        self.assertEqual((grid.nx, grid.ny, grid.nz), (2, 1, 1))
        # canonical order: cell i=0 (x in [0,1]) first -> value 3, then 7.
        self.assertEqual(fields["u"], [3.0, 7.0])


class TestSvgAndAudit(unittest.TestCase):
    def test_line_chart_is_wellformed_svg(self) -> None:
        out = svg.line_chart(
            [svg.Series("a", [0, 1, 2], [0.0, 1.0, 0.5])], title="t", xlabel="x", ylabel="y"
        )
        root = ET.fromstring(out)
        self.assertTrue(root.tag.endswith("svg"))
        self.assertIn("polyline", out)

    def test_audit_parser_aggregates_events(self) -> None:
        lines = "\n".join([
            '{"event":"step_begin","step":1,"t":0,"dt":0}',
            '{"event":"refine","step":1,"count":10,"reason":"sensor thresholds exceeded"}',
            '{"event":"acceptance","step":1,"region":"NEAR_WALL","mode":"NEAR_WALL","accepted":true,"reason":"12 accepted, 3 rejected -> FULL_NS"}',
            '{"event":"acceptance","step":1,"region":"hard-guard-zones","mode":"FULL_NS","accepted":true,"reason":"42 reduced proposals overridden to FULL_NS"}',
            '{"event":"metrics","step":1,"cells":1000,"cd":0.9,"cl":0.3}',
            '{"event":"step_end","step":1}',
        ])
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "audit.jsonl"
            p.write_text(lines + "\n")
            data = audit_mod.load_audit(p)
        self.assertEqual(data.cells, [1000])
        self.assertEqual(data.refine_per_step[1], 10)
        self.assertEqual(data.acceptance["NEAR_WALL"].accepted, 12)
        self.assertEqual(data.acceptance["NEAR_WALL"].rejected, 3)
        self.assertEqual(data.guard_overrides_total, 42)


if __name__ == "__main__":
    unittest.main()
