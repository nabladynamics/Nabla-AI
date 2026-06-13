// Audit feed: the adaptive layer's decision trail, narrated in real time.

import type { AuditEntry } from '../../hooks/useTelemetry'

const KIND_ICON: Record<AuditEntry['kind'], string> = {
  refine: '▲',
  coarsen: '▽',
  promote: '⬆',
  accept: '✓',
  guard: '⛨',
  decision: '●',
  info: '·',
}

export function AuditFeed({ entries }: { entries: AuditEntry[] }) {
  return (
    <div className="sim-panel audit-feed">
      <h4>Audit feed</h4>
      {entries.length === 0 ? (
        <p className="muted">
          Adaptive decisions stream here (refinements, model promotions, guard overrides).
        </p>
      ) : (
        <ul>
          {entries.map((entry) => (
            <li key={entry.id} className={`audit-entry audit-entry--${entry.kind}`}>
              <span className="audit-icon">{KIND_ICON[entry.kind]}</span>
              <span className="audit-step">s{entry.step}</span>
              <span className="audit-text">{entry.text}</span>
            </li>
          ))}
        </ul>
      )}
    </div>
  )
}
