// The AI's proposed experiment card rendered as EDITABLE form fields.
// The user always confirms (or overrides) — AI proposes, never silently decides.

import { useState } from 'react'

import type { ExperimentCard, TargetOutput } from '../api'

const ALL_OUTPUTS: { key: TargetOutput; label: string }[] = [
  { key: 'cd', label: 'Cd' },
  { key: 'cl', label: 'Cl' },
  { key: 'strouhal', label: 'Strouhal' },
  { key: 'reattachment_length', label: 'Reattachment' },
  { key: 'velocity_profiles', label: 'Profiles' },
  { key: 'pressure_distribution', label: 'Pressure' },
]

interface Props {
  card: ExperimentCard
  onConfirm: (edited: ExperimentCard) => void
}

export function ExperimentCardForm({ card, onConfirm }: Props) {
  const [draft, setDraft] = useState<ExperimentCard>(card)
  const [confirmed, setConfirmed] = useState(false)

  const set = <K extends keyof ExperimentCard>(key: K, value: ExperimentCard[K]) =>
    setDraft((d) => ({ ...d, [key]: value }))

  const toggleOutput = (output: TargetOutput) =>
    set(
      'target_outputs',
      draft.target_outputs.includes(output)
        ? draft.target_outputs.filter((o) => o !== output)
        : [...draft.target_outputs, output],
    )

  return (
    <div className="card-form">
      <div className="card-form-head">
        <strong>Experiment card</strong>
        <span className={`badge badge--${draft.confidence}`}>{draft.confidence} confidence</span>
      </div>
      <p className="muted card-id">{draft.object_identification}</p>

      <div className="field">
        <label>Title</label>
        <input value={draft.title} onChange={(e) => set('title', e.target.value)} />
      </div>
      <div className="field-row">
        <div className="field">
          <label>Case</label>
          <select
            value={draft.case_type}
            onChange={(e) => set('case_type', e.target.value as ExperimentCard['case_type'])}
          >
            <option value="wall-mounted-cube">wall-mounted-cube</option>
            <option value="channel">channel</option>
            <option value="lid-cavity">lid-cavity</option>
          </select>
        </div>
        <div className="field">
          <label>Reynolds</label>
          <input
            type="number"
            value={draft.reynolds_number}
            onChange={(e) => set('reynolds_number', Number(e.target.value))}
          />
        </div>
      </div>
      <div className="field-row">
        <div className="field">
          <label>Inlet velocity [m/s]</label>
          <input
            type="number"
            step="any"
            value={draft.inlet_velocity}
            onChange={(e) => set('inlet_velocity', Number(e.target.value))}
          />
        </div>
        <div className="field">
          <label>Inlet profile</label>
          <select
            value={draft.inlet_profile}
            onChange={(e) => set('inlet_profile', e.target.value as 'uniform' | 'parabolic')}
          >
            <option value="uniform">uniform</option>
            <option value="parabolic">parabolic</option>
          </select>
        </div>
      </div>
      <div className="field-row">
        <div className="field">
          <label>Resolution [cells/h]</label>
          <input
            type="number"
            value={draft.resolution}
            onChange={(e) => set('resolution', Number(e.target.value))}
          />
        </div>
        <div className="field">
          <label>Max steps</label>
          <input
            type="number"
            value={draft.max_steps}
            onChange={(e) => set('max_steps', Number(e.target.value))}
          />
        </div>
      </div>
      <div className="segmented segmented--small">
        <button type="button" className={draft.steady ? 'on' : ''} onClick={() => set('steady', true)}>
          Steady
        </button>
        <button
          type="button"
          className={!draft.steady ? 'on' : ''}
          onClick={() => set('steady', false)}
        >
          Transient
        </button>
      </div>
      <div className="check-grid check-grid--compact">
        {ALL_OUTPUTS.map((o) => (
          <label key={o.key} className="check">
            <input
              type="checkbox"
              checked={draft.target_outputs.includes(o.key)}
              onChange={() => toggleOutput(o.key)}
            />
            {o.label}
          </label>
        ))}
      </div>
      {draft.open_questions.length > 0 && (
        <div className="open-questions">
          <strong>Open points</strong>
          <ul>
            {draft.open_questions.map((q) => (
              <li key={q}>{q}</li>
            ))}
          </ul>
        </div>
      )}
      <button
        type="button"
        className="btn btn--primary"
        disabled={confirmed}
        onClick={() => {
          onConfirm(draft)
          setConfirmed(true)
        }}
      >
        {confirmed ? '✓ Applied to configuration' : 'Confirm & apply to configuration'}
      </button>
    </div>
  )
}
