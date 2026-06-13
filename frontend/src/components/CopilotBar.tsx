// ∇AI Copilot bar — chat input wired to POST /api/runs/{id}/ai/ask. Replies
// render above the bar; an experiment card arrives as an editable form.

import { useState } from 'react'

import { askCopilot, type ChatTurn, type ExperimentCard } from '../api'
import { useRunContext } from '../state/RunContext'
import { ExperimentCardForm } from './ExperimentCardForm'

interface LogEntry {
  role: 'user' | 'assistant' | 'system'
  text: string
  card?: ExperimentCard
}

export function CopilotBar() {
  const { activeRun, applyCard } = useRunContext()
  const [input, setInput] = useState('')
  const [log, setLog] = useState<LogEntry[]>([])
  const [history, setHistory] = useState<ChatTurn[]>([])
  const [pending, setPending] = useState(false)
  const [open, setOpen] = useState(false)

  const send = async () => {
    const message = input.trim()
    if (!message || !activeRun || pending) return
    setInput('')
    setOpen(true)
    setLog((entries) => [...entries, { role: 'user', text: message }])
    setPending(true)
    try {
      const response = await askCopilot(activeRun.id, message, history)
      setHistory((turns) => [
        ...turns,
        { role: 'user', content: message },
        { role: 'assistant', content: response.reply || '(experiment card)' },
      ])
      setLog((entries) => [
        ...entries,
        {
          role: 'assistant',
          text: response.reply,
          card: response.experiment_card ?? undefined,
        },
      ])
    } catch (err) {
      setLog((entries) => [
        ...entries,
        {
          role: 'system',
          text:
            err instanceof Error
              ? `Copilot unavailable: ${err.message}`
              : 'Copilot unavailable.',
        },
      ])
    } finally {
      setPending(false)
    }
  }

  return (
    <div className="copilot">
      {open && log.length > 0 && (
        <div className="copilot-log">
          {log.map((entry, index) => (
            <div key={index} className={`copilot-msg copilot-msg--${entry.role}`}>
              {entry.text && <p>{entry.text}</p>}
              {entry.card && <ExperimentCardForm card={entry.card} onConfirm={applyCard} />}
            </div>
          ))}
          {pending && <div className="copilot-msg copilot-msg--assistant">thinking…</div>}
        </div>
      )}
      <div className="copilot-bar">
        <span className="copilot-brand">∇AI Copilot</span>
        <input
          value={input}
          placeholder={
            activeRun ? 'pregunta algo… (e.g. "drag of this cube at Re 500")' : 'upload an STL first'
          }
          disabled={!activeRun || pending}
          onChange={(event) => setInput(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === 'Enter') void send()
          }}
        />
        <button
          type="button"
          className="btn btn--primary"
          disabled={!activeRun || pending || !input.trim()}
          onClick={() => void send()}
        >
          {pending ? '…' : 'Ask'}
        </button>
        {log.length > 0 && (
          <button type="button" className="chip-btn" onClick={() => setOpen((o) => !o)}>
            {open ? 'Hide' : `Show (${log.length})`}
          </button>
        )}
      </div>
    </div>
  )
}
