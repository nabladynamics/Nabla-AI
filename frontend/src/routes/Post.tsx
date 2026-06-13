// Post-simulation tab: Flow Field / Vortex Structures / Forces & Spectra /
// Exports sub-views plus the Recommendations panel.

import { useEffect, useRef, useState } from 'react'

import {
  fetchAnalysis,
  fetchField,
  listSnapshots,
  type AnalysisResponse,
} from '../api'
import { FlowFieldView } from '../components/post/FlowFieldView'
import { ForcesView } from '../components/post/ForcesView'
import { ExportsView } from '../components/post/ExportsView'
import { RecommendationsPanel } from '../components/post/RecommendationsPanel'
import { VortexView } from '../components/post/VortexView'
import { decodeBundle, type Field } from '../components/post/vtkUtils'
import { useRunContext } from '../state/RunContext'

const SUB_VIEWS = ['Flow Field', 'Vortex Structures', 'Forces & Spectra', 'Exports'] as const
type SubView = (typeof SUB_VIEWS)[number]

export function Post() {
  const { activeRun, report } = useRunContext()
  const [view, setView] = useState<SubView>('Flow Field')
  const [steps, setSteps] = useState<number[]>([])
  const [currentStep, setCurrentStep] = useState(-1) // -1 = final
  const [field, setField] = useState<Field | null>(null)
  const [fieldError, setFieldError] = useState<string | null>(null)
  const [fieldLoading, setFieldLoading] = useState(false)
  const [analysis, setAnalysis] = useState<AnalysisResponse | null>(null)
  const [windowFrac, setWindowFrac] = useState(0.5)
  const fieldCache = useRef(new Map<number, Field>())

  const runId = activeRun?.id ?? null

  useEffect(() => {
    fieldCache.current.clear()
    setField(null)
    setAnalysis(null)
    setSteps([])
    setCurrentStep(-1)
    setFieldError(null)
    if (!runId) return
    void listSnapshots(runId)
      .then((response) => setSteps(response.steps))
      .catch(() => setSteps([]))
  }, [runId])

  // field for the scrubbed step (cached)
  useEffect(() => {
    if (!runId) return
    const cached = fieldCache.current.get(currentStep)
    if (cached) {
      setField(cached)
      setFieldError(null)
      return
    }
    setFieldLoading(true)
    void fetchField(runId, currentStep)
      .then((bundle) => {
        const decoded = decodeBundle(bundle)
        fieldCache.current.set(currentStep, decoded)
        setField(decoded)
        setFieldError(null)
      })
      .catch((err: Error) => {
        setField(null)
        setFieldError(err.message)
      })
      .finally(() => setFieldLoading(false))
  }, [runId, currentStep])

  // analysis (re-runs when the statistics window changes)
  useEffect(() => {
    if (!runId) return
    const timer = setTimeout(() => {
      void fetchAnalysis(runId, windowFrac)
        .then(setAnalysis)
        .catch(() => setAnalysis(null))
    }, 250)
    return () => clearTimeout(timer)
  }, [runId, windowFrac])

  if (!activeRun) {
    return (
      <section className="stage">
        <h2>Post-simulation</h2>
        <p className="muted">No run selected — pick one in the run selector.</p>
      </section>
    )
  }

  return (
    <div className="post-dash">
      <div className="post-tabs">
        {SUB_VIEWS.map((name) => (
          <button
            key={name}
            type="button"
            className={`post-tab ${view === name ? 'post-tab--active' : ''}`}
            onClick={() => setView(name)}
          >
            {name}
          </button>
        ))}
        <span className="muted post-run-label">
          {activeRun.name} · {activeRun.status}
          {fieldError ? ` · ${fieldError}` : ''}
        </span>
      </div>
      <div className="post-grid">
        <div className="post-main">
          {view === 'Flow Field' && (
            <FlowFieldView
              field={field}
              steps={steps}
              currentStep={currentStep}
              onStepChange={setCurrentStep}
              report={report}
              loading={fieldLoading}
              error={fieldError}
            />
          )}
          {view === 'Vortex Structures' && (
            <VortexView
              field={field}
              analysis={analysis}
              report={report}
              loading={fieldLoading}
              error={fieldError}
            />
          )}
          {view === 'Forces & Spectra' && (
            <ForcesView
              runId={activeRun.id}
              analysis={analysis}
              windowFrac={windowFrac}
              onWindowChange={setWindowFrac}
            />
          )}
          {view === 'Exports' && <ExportsView runId={activeRun.id} />}
        </div>
        <RecommendationsPanel analysis={analysis} />
      </div>
    </div>
  )
}
