// "Report inicial" — imposed physical conditions + derived quantities. Mesh
// numbers come straight from the backend's geometry report; turbulence scales
// are the standard isotropic estimates from the imposed Re (labeled as such).

import { useRunContext } from '../../state/RunContext'

function fmt(value: number | undefined, digits = 3): string {
  if (value === undefined || Number.isNaN(value)) return '—'
  if (value !== 0 && (Math.abs(value) < 1e-2 || Math.abs(value) >= 1e5)) {
    return value.toExponential(2)
  }
  return value.toLocaleString(undefined, { maximumSignificantDigits: digits })
}

export function ReportPanel() {
  const { report, config, fluid, transient } = useRunContext()
  if (!report) {
    return (
      <section className="panel panel--report">
        <h3>Initial report</h3>
        <p className="muted">Upload an STL to generate the geometry & mesh report.</p>
      </section>
    )
  }

  const h = report.bounding_box.size[1] || 1
  const re = config.reynolds
  const kolmogorov = h * Math.pow(re, -0.75)
  const taylor = h * Math.sqrt(10) * Math.pow(re, -0.5)
  const minDelta = report.mesh.edge_target_size ?? report.mesh.surface_target_size
  const nu = (fluid.uInf * h) / re

  return (
    <section className="panel panel--report">
      <h3>Initial report</h3>

      <h4>Imposed conditions</h4>
      <dl className="kv">
        <dt>Case</dt>
        <dd>{config.type}</dd>
        <dt>U∞ inlet</dt>
        <dd>{fmt(fluid.uInf)} m/s</dd>
        <dt>Regime</dt>
        <dd>{transient ? 'transient' : 'steady target'}</dd>
        <dt>ρ / μ</dt>
        <dd>
          {fmt(fluid.rho)} kg/m³ · {fmt(fluid.mu)} Pa·s
        </dd>
      </dl>

      <h4>Derived quantities</h4>
      <dl className="kv">
        <dt>Reynolds (Re_h)</dt>
        <dd>
          <strong>{fmt(re, 5)}</strong>
        </dd>
        <dt>ν effective</dt>
        <dd>{fmt(nu)} m²/s</dd>
        <dt>Kolmogorov η/h</dt>
        <dd>{fmt(kolmogorov / h)} (estimate)</dd>
        <dt>Taylor λ/h</dt>
        <dd>{fmt(taylor / h)} (estimate)</dd>
      </dl>

      <h4>Initial mesh</h4>
      <dl className="kv">
        <dt>Cells</dt>
        <dd>{report.mesh.cells.toLocaleString()}</dd>
        <dt>min Δ</dt>
        <dd>{fmt(minDelta)} m</dd>
        <dt>Δ/h</dt>
        <dd>{minDelta ? fmt(minDelta / h) : '—'}</dd>
        <dt>Levels</dt>
        <dd>
          {report.mesh.min_level ?? '—'} – {report.mesh.max_level ?? '—'}
          {report.mesh.balanced ? ' (2:1 balanced)' : ''}
        </dd>
        <dt>Sharp edges</dt>
        <dd>{report.features.sharp_edges.count} (extra refinement)</dd>
        <dt>Watertight</dt>
        <dd>{report.cleaning.watertight ? 'yes' : 'NO — check geometry'}</dd>
      </dl>

      <h4>Solver method</h4>
      <ul className="method-list">
        <li>RK3 fractional-step projection</li>
        <li>WENO-5 upwind convection</li>
        <li>2nd-order FV diffusion</li>
        <li>Sparse pressure solve (CG, direct-ready)</li>
        <li>Log-law wall model (adaptive layer)</li>
      </ul>
    </section>
  )
}
