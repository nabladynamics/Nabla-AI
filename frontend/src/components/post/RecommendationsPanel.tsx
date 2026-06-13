// Recommendations: the audit-trail summary (reduced physics accepted vs
// rejected, achieved speedup vs uniform baseline) and unconverged-metric
// warnings — the platform tells you how much it trusted, and what to check.

import type { AnalysisResponse } from '../../api'

export function RecommendationsPanel({ analysis }: { analysis: AnalysisResponse | null }) {
  if (!analysis) {
    return (
      <aside className="post-recs">
        <h4>Recommendations</h4>
        <p className="muted">analysis loading…</p>
      </aside>
    )
  }
  const audit = analysis.audit
  const total = audit ? audit.accepts + audit.rejects : 0

  return (
    <aside className="post-recs">
      <h4>Recommendations</h4>

      {audit ? (
        <div className="rec-card rec-card--audit">
          <strong>Audited model decisions</strong>
          <dl className="kv">
            <dt>reduced-physics accepted</dt>
            <dd>
              {audit.accepts.toLocaleString()}
              {total > 0 ? ` (${((100 * audit.accepts) / total).toFixed(1)}%)` : ''}
            </dd>
            <dt>rejected → FULL_NS</dt>
            <dd>{audit.rejects.toLocaleString()}</dd>
            <dt>hard-guard overrides</dt>
            <dd>{audit.guard_overrides.toLocaleString()}</dd>
            <dt>refinements / coarsenings</dt>
            <dd>
              {audit.refinements.toLocaleString()} / {audit.coarsenings.toLocaleString()}
            </dd>
          </dl>
          {audit.cell_speedup !== null && (
            <p className="rec-speedup">
              <strong>{audit.cell_speedup.toFixed(1)}×</strong> fewer cells than the
              uniform-fine baseline
              {audit.adaptive_mean_cells !== null && audit.uniform_fine_cells !== null
                ? ` (${Math.round(audit.adaptive_mean_cells).toLocaleString()} vs ${Math.round(audit.uniform_fine_cells).toLocaleString()})`
                : ''}
            </p>
          )}
        </div>
      ) : (
        <div className="rec-card">
          <strong>No audit trail</strong>
          <p className="muted">
            This was a uniform-physics run. Launch with the adaptive solve enabled to get
            audited reduced-physics decisions and the speedup report.
          </p>
        </div>
      )}

      {analysis.warnings.length > 0 && (
        <div className="rec-card rec-card--warn">
          <strong>Checks & warnings</strong>
          <ul>
            {analysis.warnings.map((warning) => (
              <li key={warning}>{warning}</li>
            ))}
          </ul>
        </div>
      )}

      {analysis.recirculation && (
        <div className="rec-card">
          <strong>Recirculation summary</strong>
          <dl className="kv">
            <dt>upstream separation</dt>
            <dd>
              {analysis.recirculation.upstream_separation_over_h !== null
                ? `${analysis.recirculation.upstream_separation_over_h.toFixed(2)} h`
                : 'not detected'}
            </dd>
            <dt>reattachment length</dt>
            <dd>
              {analysis.recirculation.reattachment_over_h !== null
                ? `${analysis.recirculation.reattachment_over_h.toFixed(2)} h`
                : 'not detected'}
            </dd>
            <dt>reverse-flow volume</dt>
            <dd>
              {analysis.recirculation.reverse_flow_volume_over_h3 !== null
                ? `${analysis.recirculation.reverse_flow_volume_over_h3.toFixed(2)} h³`
                : '—'}
            </dd>
          </dl>
        </div>
      )}
    </aside>
  )
}
