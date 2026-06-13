import { useRunContext, type OutputsState } from '../../state/RunContext'

const OPTIONS: { key: keyof OutputsState; label: string }[] = [
  { key: 'cd', label: 'Drag coefficient (Cd)' },
  { key: 'cl', label: 'Lift coefficient (Cl)' },
  { key: 'wake', label: 'Wake & recirculation' },
  { key: 'spectra', label: 'Spectra / Strouhal' },
]

export function OutputsPanel() {
  const { outputs, setOutputs } = useRunContext()
  return (
    <section className="panel">
      <h3>Target outputs</h3>
      <div className="check-grid">
        {OPTIONS.map((option) => (
          <label key={option.key} className="check">
            <input
              type="checkbox"
              checked={outputs[option.key]}
              onChange={(event) => setOutputs({ [option.key]: event.target.checked })}
            />
            {option.label}
          </label>
        ))}
      </div>
    </section>
  )
}
