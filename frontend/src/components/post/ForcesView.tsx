// Forces & Spectra sub-view: Cd/Cl/F histories with a statistics-window
// selector, lift PSD with the detected Strouhal peak annotated, and the
// comparison table against the validation reference registry.

import { useEffect, useMemo, useState } from 'react'

import { artifactUrl, type AnalysisResponse } from '../../api'
import { ChartPanel } from '../sim/ChartPanel'

interface DiagRecord {
  step: number
  t: number
  cd: number
  cl: number
}

interface Props {
  runId: string
  analysis: AnalysisResponse | null
  windowFrac: number
  onWindowChange: (value: number) => void
}

const STATUS_COLOR: Record<string, string> = {
  PASS: '#1e8449',
  MARGINAL: '#b9770e',
  FAIL: '#c0392b',
  'NO-REF (TODO)': '#5b6b86',
  'N/A': '#3a4862',
}

export function ForcesView({ runId, analysis, windowFrac, onWindowChange }: Props) {
  const [records, setRecords] = useState<DiagRecord[]>([])

  useEffect(() => {
    void fetch(artifactUrl(runId, 'diagnostics.jsonl'))
      .then((response) => (response.ok ? response.text() : Promise.reject(new Error('no diagnostics'))))
      .then((text) => {
        const byStep = new Map<number, DiagRecord>()
        for (const line of text.split('\n')) {
          if (!line.trim()) continue
          const record = JSON.parse(line) as DiagRecord
          if (typeof record.step !== 'number') continue // provenance meta line
          byStep.set(record.step, record)
        }
        setRecords([...byStep.values()].sort((a, b) => a.step - b.step))
      })
      .catch(() => setRecords([]))
  }, [runId])

  const windowStart = records.length ? records[records.length - 1].step * (1 - windowFrac) : 0
  const inWindow = useMemo(() => records.filter((r) => r.step >= windowStart), [records, windowStart])

  const forcesData = useMemo(
    () =>
      [
        records.map((r) => r.t),
        records.map((r) => r.cd),
        records.map((r) => r.cl),
        records.map((r) => 0.5 * r.cd),
        records.map((r) => 0.5 * r.cl),
      ] as [number[], number[], number[], number[], number[]],
    [records],
  )

  const spectrum = analysis?.spectrum
  const psdData = useMemo(
    () => [spectrum?.st ?? [], spectrum?.mag ?? []] as [number[], number[]],
    [spectrum],
  )

  const stats = useMemo(() => {
    if (!inWindow.length) return null
    const mean = (values: number[]) => values.reduce((a, b) => a + b, 0) / values.length
    const std = (values: number[], m: number) =>
      Math.sqrt(values.reduce((a, b) => a + (b - m) ** 2, 0) / values.length)
    const cd = inWindow.map((r) => r.cd)
    const cl = inWindow.map((r) => r.cl)
    const cdMean = mean(cd)
    const clMean = mean(cl)
    return {
      n: inWindow.length,
      cdMean,
      cdStd: std(cd, cdMean),
      clMean,
      clStd: std(cl, clMean),
    }
  }, [inWindow])

  return (
    <div className="post-view forces-view">
      <div className="post-toolbar">
        <label className="slider-label">
          statistics window: last {(windowFrac * 100).toFixed(0)}%
          <input
            type="range"
            min={0.1}
            max={1}
            step={0.05}
            value={windowFrac}
            onChange={(e) => onWindowChange(Number(e.target.value))}
          />
        </label>
        {stats && (
          <span className="muted">
            window: {stats.n} steps · Cd = {stats.cdMean.toFixed(4)} ± {stats.cdStd.toFixed(4)} ·
            Cl = {stats.clMean.toFixed(4)} ± {stats.clStd.toFixed(4)}
          </span>
        )}
      </div>

      <ChartPanel
        title="Cd / Cl and forces (F = 0.5·C in solver units) — shaded values feed the statistics"
        xLabel="t"
        series={[
          { label: 'Cd', color: '#e0455e' },
          { label: 'Cl', color: '#3f7fd6' },
          { label: 'F_D', color: '#ffb3c0', dash: [4, 4] },
          { label: 'F_L', color: '#9cc3f0', dash: [4, 4] },
        ]}
        data={forcesData}
        height={220}
      />

      <ChartPanel
        title={
          spectrum?.prominent && spectrum.strouhal !== null
            ? `Lift PSD — Strouhal peak at St = ${spectrum.strouhal.toFixed(4)} (±${spectrum.resolution_st.toFixed(3)}, SNR ${spectrum.snr.toFixed(1)}, ${spectrum.cycles.toFixed(1)} cycles)`
            : 'Lift PSD — no dominant shedding peak'
        }
        xLabel="St = f·h/U"
        series={[{ label: '|Ĉl|', color: '#2dbd9b' }]}
        data={psdData}
        height={200}
        vlines={
          spectrum?.prominent && spectrum.strouhal !== null
            ? [{ x: spectrum.strouhal, label: `St=${spectrum.strouhal.toFixed(3)}`, color: '#f0883e' }]
            : []
        }
      />
      {spectrum && !spectrum.prominent && (
        <p className="muted spectrum-note">
          {spectrum.reason}.{' '}
          {spectrum.window_too_short
            ? `Need ≥ ${spectrum.min_cycles.toFixed(0)} shedding cycles in the window — run longer or widen the statistics window.`
            : 'A non-detection at Re_h = 500 is physically expected: the wall-mounted-cube wake is steady or only weakly unsteady here. Clean shedding peaks appear higher up the Reynolds ladder.'}
        </p>
      )}

      <div className="sim-panel">
        <h4>
          Comparison vs reference registry
          {analysis?.reference_re !== null && analysis?.reference_re !== undefined
            ? ` (rung re${String(analysis.reference_re).padStart(4, '0')})`
            : ''}
        </h4>
        <p className="muted spectrum-note">
          Rows stay <code>NO-REF (TODO)</code> until literature values are digitized into the
          reference registry; the pass/fail bands light up automatically once they are. Reference
          numbers are never fabricated to fill the table.
        </p>
        <table className="compare-table">
          <thead>
            <tr>
              <th>metric</th>
              <th>computed</th>
              <th>reference</th>
              <th>band</th>
              <th>status</th>
            </tr>
          </thead>
          <tbody>
            {(analysis?.comparison ?? []).map((row) => (
              <tr key={row.metric}>
                <td>
                  <code>{row.metric}</code>
                </td>
                <td>{row.computed !== null ? row.computed.toPrecision(4) : '—'}</td>
                <td>{row.reference !== null ? row.reference.toPrecision(4) : 'TODO'}</td>
                <td>
                  {(row.band[0] * 100).toFixed(0)}–{(row.band[1] * 100).toFixed(0)}%
                </td>
                <td>
                  <span
                    className="chip"
                    style={{ background: STATUS_COLOR[row.status] ?? '#5b6b86' }}
                  >
                    {row.status}
                  </span>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
