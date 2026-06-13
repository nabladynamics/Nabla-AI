// Simulation tab — live diagnostics dashboard (dark): GLOBAL charts on the
// left, the current-Δt LOCAL panel beneath them, the Adaptive Fidelity Map and
// Audit feed on the right, all fed by the telemetry WebSocket.

import { useMemo } from 'react'

import { pauseRun, resumeRun } from '../api'
import { ChartPanel } from '../components/sim/ChartPanel'
import { AuditFeed } from '../components/sim/AuditFeed'
import { FidelityMap } from '../components/sim/FidelityMap'
import { useTelemetry } from '../hooks/useTelemetry'
import { useRunContext } from '../state/RunContext'

function fmt(value: number | null | undefined, digits = 4): string {
  if (value === null || value === undefined || Number.isNaN(value)) return '—'
  if (value !== 0 && (Math.abs(value) < 1e-3 || Math.abs(value) >= 1e5)) {
    return value.toExponential(2)
  }
  return value.toLocaleString(undefined, { maximumSignificantDigits: digits })
}

export function Sim() {
  const { activeRun, refreshRuns, selectRun } = useRunContext()
  const telemetry = useTelemetry(activeRun?.id ?? null)
  const { series, fractions, local, feed, status, rev } = telemetry

  const isAdaptive = Boolean(activeRun?.config.adaptive)
  const targetSteps =
    (activeRun?.config.max_steps ?? 0) +
    (isAdaptive ? (activeRun?.config.adaptive_warmup ?? 0) : 0)
  const lastStep = series.step.at(-1) ?? 0
  const lastT = series.t.at(-1) ?? 0
  const progress = targetSteps > 0 ? Math.min(lastStep / targetSteps, 1) : 0
  const liveStatus = status ?? activeRun?.status ?? null
  const running = liveStatus === 'running'

  // The telemetry series/fractions arrays are mutated in place (stable object
  // identity), so `rev` — bumped on every WS message — is what makes these
  // memos recompute and hand a fresh AlignedData reference to uPlot. Without it
  // the charts would setData once with empty arrays and never update again.
  const forceScale = 0.5 // 0.5 * rho * U^2 * A with solver units rho=U=A=1
  const forcesData = useMemo(
    () =>
      [series.t, series.cd, series.cl, series.cdMean, series.clMean] as [
        number[],
        number[],
        number[],
        number[],
        number[],
      ],
    [series, rev],
  )
  const residualData = useMemo(
    () => [series.t, series.momentum, series.continuity] as [number[], number[], number[]],
    [series, rev],
  )
  const cflData = useMemo(
    () => [series.t, series.cfl, series.dt] as [number[], number[], number[]],
    [series, rev],
  )
  const cellsData = useMemo(() => [series.t, series.cells] as [number[], number[]], [series, rev])
  const innerData = useMemo(
    () => [series.step, series.poissonIters] as [number[], number[]],
    [series, rev],
  )
  const fractionData = useMemo(() => {
    const steps = fractions.map((f) => f.step)
    const cheap = fractions.map((f) => f.cheap * 100)
    const full = fractions.map(() => 100) // stacked: full series drawn underneath at 100%
    return [steps, full, cheap] as [number[], number[], number[]]
  }, [fractions, rev])

  if (!activeRun) {
    return (
      <section className="stage">
        <h2>Simulation</h2>
        <p className="muted">No run selected — define and launch one in Pre-simulation.</p>
      </section>
    )
  }

  return (
    <div className="sim-dash">
      {/* ---- global stat bar -------------------------------------------- */}
      <div className="sim-statbar">
        <div className="stat">
          <label>run</label>
          <strong>{activeRun.name}</strong>
        </div>
        <div className="stat">
          <label>status</label>
          <strong className={`status status--${liveStatus ?? 'unknown'}`}>
            {liveStatus ?? '—'}
            {!telemetry.connected && !telemetry.eof ? ' (reconnecting…)' : ''}
          </strong>
        </div>
        <div className="stat">
          <label>step</label>
          <strong>
            {lastStep} / {targetSteps || '?'}
          </strong>
        </div>
        <div className="stat">
          <label>physical time</label>
          <strong>{fmt(lastT)} h/U</strong>
        </div>
        <div className="stat stat--progress">
          <label>progress</label>
          <div className="progress">
            <div className="progress-fill" style={{ width: `${progress * 100}%` }} />
          </div>
          <strong>{Math.round(progress * 100)}%</strong>
        </div>
        <div className="sim-controls">
          <button
            type="button"
            className="btn"
            disabled={!running}
            onClick={() => {
              void pauseRun(activeRun.id).then(() => refreshRuns().then(() => selectRun(activeRun.id)))
            }}
          >
            {isAdaptive ? '■ Stop' : '⏸ Pause'}
          </button>
          <button
            type="button"
            className="btn"
            disabled={liveStatus !== 'paused' || isAdaptive}
            title={isAdaptive ? 'adaptive runs cannot resume in Phase 0' : 'resume from checkpoint'}
            onClick={() => {
              void resumeRun(activeRun.id).then(() =>
                refreshRuns().then(() => selectRun(activeRun.id)),
              )
            }}
          >
            ▶ Resume
          </button>
        </div>
      </div>

      <div className="sim-grid">
        {/* ---- GLOBAL charts ------------------------------------------- */}
        <div className="sim-charts">
          <ChartPanel
            title={`Force coefficients · F_D = ${fmt((series.cd.at(-1) ?? 0) * forceScale)}, F_L = ${fmt((series.cl.at(-1) ?? 0) * forceScale)} (solver units)`}
            xLabel="t"
            series={[
              { label: 'Cd', color: '#e0455e' },
              { label: 'Cl', color: '#3f7fd6' },
              { label: 'Cd mean', color: '#ffb3c0', dash: [6, 4] },
              { label: 'Cl mean', color: '#9cc3f0', dash: [6, 4] },
            ]}
            data={forcesData}
          />
          <ChartPanel
            title="Residual histories"
            xLabel="t"
            logY
            series={[
              { label: 'momentum', color: '#9d5bd2' },
              { label: 'continuity', color: '#2dbd9b' },
            ]}
            data={residualData}
          />
          <div className="sim-chart-row">
            <ChartPanel
              title="CFL & Δt"
              xLabel="t"
              series={[
                { label: 'CFL target', color: '#f0883e' },
                { label: 'Δt', color: '#46a5c9' },
              ]}
              data={cflData}
              height={140}
            />
            <ChartPanel
              title="Cell count"
              xLabel="t"
              series={[{ label: 'cells', color: '#aee0e6', fill: 'rgba(70,165,201,0.18)' }]}
              data={cellsData}
              height={140}
            />
          </div>

          {/* ---- LOCAL (current Δt) ------------------------------------ */}
          <div className="sim-panel sim-local">
            <h4>Current Δt</h4>
            <div className="local-stats">
              <div className="stat">
                <label>CFL</label>
                <strong>{fmt(local?.cfl)}</strong>
              </div>
              <div className="stat">
                <label>Δt</label>
                <strong>{fmt(local?.dt)}</strong>
              </div>
              <div className="stat">
                <label>inner iters</label>
                <strong>{local?.poissonIters ?? '—'}</strong>
              </div>
              <div className="stat">
                <label>Cd / Cl (inst.)</label>
                <strong>
                  {fmt(local?.cd)} / {fmt(local?.cl)}
                </strong>
              </div>
              <div className="stat">
                <label>refined / coarsened</label>
                <strong>
                  {local?.refined ?? 0} / {local?.coarsened ?? 0}
                </strong>
              </div>
              <div className="stat">
                <label>cheap physics</label>
                <strong>
                  {local?.cheapFrac !== null && local?.cheapFrac !== undefined
                    ? `${(local.cheapFrac * 100).toFixed(1)}%`
                    : '—'}
                </strong>
              </div>
            </div>
            <div className="sim-chart-row">
              <ChartPanel
                title="Inner iterations (pressure solve) per step"
                xLabel="step"
                series={[{ label: 'iters', color: '#f0883e' }]}
                data={innerData}
                height={120}
              />
              <ChartPanel
                title="Cheap physics vs DNS cells [%]"
                xLabel="step"
                series={[
                  { label: 'FULL_NS', color: '#e0455e', fill: 'rgba(224,69,94,0.35)' },
                  { label: 'cheap models', color: '#2dbd9b', fill: 'rgba(45,189,155,0.45)' },
                ]}
                data={fractionData}
                height={120}
              />
            </div>
          </div>
        </div>

        {/* ---- right column: fidelity map + audit feed ------------------ */}
        <div className="sim-side">
          <FidelityMap runId={activeRun.id} live={running} />
          <AuditFeed entries={feed} />
        </div>
      </div>
    </div>
  )
}
