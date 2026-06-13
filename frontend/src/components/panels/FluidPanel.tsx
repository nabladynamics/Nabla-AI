import { reynolds, useRunContext } from '../../state/RunContext'

const PRESETS = {
  demo: { label: 'Demo fluid (Re 500)', rho: 1.0, mu: 0.002, uInf: 1.0 },
  air: { label: 'Air 15 °C', rho: 1.225, mu: 1.81e-5, uInf: 1.0 },
  water: { label: 'Water 20 °C', rho: 998, mu: 1.0e-3, uInf: 0.5 },
} as const

export function FluidPanel() {
  const { fluid, setFluid, transient, setTransient, report } = useRunContext()
  const h = report?.bounding_box.size[1] ?? 1
  const re = reynolds(fluid, h)

  return (
    <section className="panel">
      <h3>Fluid properties</h3>
      <div className="field">
        <label htmlFor="preset">Preset</label>
        <select
          id="preset"
          defaultValue="demo"
          onChange={(event) => {
            const preset = PRESETS[event.target.value as keyof typeof PRESETS]
            setFluid({ rho: preset.rho, mu: preset.mu, uInf: preset.uInf })
          }}
        >
          {Object.entries(PRESETS).map(([key, preset]) => (
            <option key={key} value={key}>
              {preset.label}
            </option>
          ))}
        </select>
      </div>
      <div className="field-row">
        <div className="field">
          <label htmlFor="rho">ρ — density [kg/m³]</label>
          <input
            id="rho"
            type="number"
            step="any"
            value={fluid.rho}
            onChange={(event) => setFluid({ rho: Number(event.target.value) })}
          />
        </div>
        <div className="field">
          <label htmlFor="mu">μ — viscosity [Pa·s]</label>
          <input
            id="mu"
            type="number"
            step="any"
            value={fluid.mu}
            onChange={(event) => setFluid({ mu: Number(event.target.value) })}
          />
        </div>
      </div>
      <div className="field">
        <label htmlFor="uinf">U∞ — free-stream velocity [m/s]</label>
        <input
          id="uinf"
          type="number"
          step="any"
          value={fluid.uInf}
          onChange={(event) => setFluid({ uInf: Number(event.target.value) })}
        />
      </div>
      <p className="derived">
        Re = ρ·U∞·h/μ = <strong>{re.toLocaleString(undefined, { maximumFractionDigits: 0 })}</strong>
        <span className="muted"> (h = {h.toPrecision(3)} m)</span>
      </p>
      <div className="segmented">
        <button
          type="button"
          className={!transient ? 'on' : ''}
          onClick={() => setTransient(false)}
        >
          Steady
        </button>
        <button type="button" className={transient ? 'on' : ''} onClick={() => setTransient(true)}>
          Transient
        </button>
      </div>
    </section>
  )
}
