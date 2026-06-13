// Advanced numerics live behind an expander — sensible defaults stay visible
// elsewhere; this is the "no overload" progressive-disclosure drawer.

import { useRunContext } from '../../state/RunContext'

export function NumericsPanel() {
  const { config, updateConfig } = useRunContext()
  return (
    <details className="panel panel--expander">
      <summary>Advanced numerics</summary>
      <label className="check check--feature">
        <input
          type="checkbox"
          checked={config.adaptive}
          onChange={(event) => updateConfig({ adaptive: event.target.checked })}
        />
        Adaptive solve (physics-aware AMR + audited model decisions)
      </label>
      {config.adaptive && (
        <div className="field">
          <label htmlFor="warmup">Warmup steps (baseline, before adapting)</label>
          <input
            id="warmup"
            type="number"
            min={0}
            value={config.adaptive_warmup}
            onChange={(event) => updateConfig({ adaptive_warmup: Number(event.target.value) })}
          />
        </div>
      )}
      <div className="field-row">
        <div className="field">
          <label htmlFor="resolution">Resolution [cells/h]</label>
          <input
            id="resolution"
            type="number"
            min={2}
            max={64}
            value={config.resolution}
            onChange={(event) => updateConfig({ resolution: Number(event.target.value) })}
          />
        </div>
        <div className="field">
          <label htmlFor="max-steps">Max steps</label>
          <input
            id="max-steps"
            type="number"
            min={1}
            value={config.max_steps}
            onChange={(event) => updateConfig({ max_steps: Number(event.target.value) })}
          />
        </div>
      </div>
      <div className="field-row">
        <div className="field">
          <label htmlFor="cfl">Target CFL</label>
          <input
            id="cfl"
            type="number"
            step="0.05"
            min={0.1}
            max={1.4}
            value={config.cfl}
            onChange={(event) => updateConfig({ cfl: Number(event.target.value) })}
          />
        </div>
        <div className="field">
          <label htmlFor="convection">Convection</label>
          <select
            id="convection"
            value={config.convection}
            onChange={(event) =>
              updateConfig({ convection: event.target.value as 'weno5' | 'central' })
            }
          >
            <option value="weno5">WENO-5 upwind</option>
            <option value="central">2nd-order central</option>
          </select>
        </div>
      </div>
      <div className="field-row">
        <div className="field">
          <label htmlFor="snap">Snapshot every</label>
          <input
            id="snap"
            type="number"
            min={0}
            value={config.snapshot_every}
            onChange={(event) => updateConfig({ snapshot_every: Number(event.target.value) })}
          />
        </div>
        <div className="field">
          <label htmlFor="ckpt">Checkpoint every</label>
          <input
            id="ckpt"
            type="number"
            min={1}
            value={config.checkpoint_every}
            onChange={(event) => updateConfig({ checkpoint_every: Number(event.target.value) })}
          />
        </div>
      </div>
    </details>
  )
}
