import { NavLink } from 'react-router-dom'

import { useRunContext } from '../state/RunContext'

const TABS = [
  { to: '/pre', label: 'Pre-simulation' },
  { to: '/sim', label: 'Simulation' },
  { to: '/post', label: 'Post-simulation' },
]

const STATUS_DOT: Record<string, string> = {
  created: '#94a3b8',
  meshing: '#eab308',
  running: '#2563eb',
  paused: '#f59e0b',
  completed: '#16a34a',
  failed: '#dc2626',
}

export function TopBar() {
  const { runs, activeRun, selectRun } = useRunContext()

  return (
    <header className="topbar">
      <span className="brand">
        <span className="brand-nabla">∇</span> Nabla AI
      </span>
      <nav className="tabs">
        {TABS.map((tab) => (
          <NavLink
            key={tab.to}
            to={tab.to}
            className={({ isActive }) => `tab ${isActive ? 'tab--active' : ''}`}
          >
            {tab.label}
          </NavLink>
        ))}
      </nav>
      <div className="run-selector">
        <label htmlFor="run-select">Run</label>
        <select
          id="run-select"
          value={activeRun?.id ?? ''}
          onChange={(event) => void selectRun(event.target.value)}
        >
          <option value="" disabled>
            {runs.length ? 'select a run…' : 'no runs yet'}
          </option>
          {runs.map((run) => (
            <option key={run.id} value={run.id}>
              {run.name} · {run.status}
            </option>
          ))}
        </select>
        {activeRun && (
          <span
            className="status-dot"
            title={activeRun.status}
            style={{ background: STATUS_DOT[activeRun.status] ?? '#94a3b8' }}
          />
        )}
      </div>
    </header>
  )
}
