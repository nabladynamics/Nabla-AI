// Shared pre-simulation state: runs list, active run, geometry, config draft.
// AI proposes (experiment card) — the user always confirms before anything
// reaches the solver config.

import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from 'react'

import {
  DEFAULT_CONFIG,
  createRun,
  fetchArtifactBuffer,
  fetchArtifactJson,
  listRuns,
  startRun,
  type CaseConfig,
  type ExperimentCard,
  type GeometryReport,
  type RunInfo,
  type TargetOutput,
} from '../api'

export interface FluidState {
  rho: number // density [kg/m^3]
  mu: number // dynamic viscosity [Pa s]
  uInf: number // free-stream velocity [m/s]
}

export interface OutputsState {
  cd: boolean
  cl: boolean
  wake: boolean
  spectra: boolean
}

interface RunContextValue {
  runs: RunInfo[]
  activeRun: RunInfo | null
  report: GeometryReport | null
  stlBuffer: ArrayBuffer | null
  config: CaseConfig
  fluid: FluidState
  outputs: OutputsState
  transient: boolean
  busy: string | null
  error: string | null
  refreshRuns: () => Promise<void>
  selectRun: (id: string) => Promise<void>
  uploadStl: (file: File) => Promise<void>
  updateConfig: (patch: Partial<CaseConfig>) => void
  setFluid: (patch: Partial<FluidState>) => void
  setOutputs: (patch: Partial<OutputsState>) => void
  setTransient: (transient: boolean) => void
  applyCard: (card: ExperimentCard) => void
  launch: () => Promise<RunInfo>
  clearError: () => void
}

const RunContext = createContext<RunContextValue | null>(null)

const DEMO_FLUID: FluidState = { rho: 1.0, mu: 0.002, uInf: 1.0 } // Re_h = 500 at h = 1

function charLength(report: GeometryReport | null): number {
  return report?.bounding_box.size[1] ?? 1.0
}

export function reynolds(fluid: FluidState, h: number): number {
  return (fluid.rho * fluid.uInf * h) / fluid.mu
}

function outputsFromCard(targets: TargetOutput[]): OutputsState {
  return {
    cd: targets.includes('cd'),
    cl: targets.includes('cl'),
    wake: targets.includes('reattachment_length') || targets.includes('velocity_profiles'),
    spectra: targets.includes('strouhal'),
  }
}

export function RunProvider({ children }: { children: ReactNode }) {
  const [runs, setRuns] = useState<RunInfo[]>([])
  const [activeRun, setActiveRun] = useState<RunInfo | null>(null)
  const [report, setReport] = useState<GeometryReport | null>(null)
  const [stlBuffer, setStlBuffer] = useState<ArrayBuffer | null>(null)
  const [config, setConfig] = useState<CaseConfig>(DEFAULT_CONFIG)
  const [fluid, setFluidState] = useState<FluidState>(DEMO_FLUID)
  const [outputs, setOutputsState] = useState<OutputsState>({
    cd: true,
    cl: true,
    wake: true,
    spectra: false,
  })
  const [transient, setTransientState] = useState(true)
  const [busy, setBusy] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  const refreshRuns = useCallback(async () => {
    try {
      setRuns(await listRuns())
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err))
    }
  }, [])

  useEffect(() => {
    void refreshRuns()
  }, [refreshRuns])

  const loadRunAssets = useCallback(async (run: RunInfo) => {
    if (run.artifacts.geometry_report) {
      setReport(await fetchArtifactJson<GeometryReport>(run.id, run.artifacts.geometry_report))
    } else {
      setReport(null)
    }
    if (run.artifacts.stl) {
      setStlBuffer(await fetchArtifactBuffer(run.id, run.artifacts.stl))
    } else {
      setStlBuffer(null)
    }
  }, [])

  const selectRun = useCallback(
    async (id: string) => {
      const run = runs.find((r) => r.id === id)
      if (!run) return
      setBusy('loading run…')
      setError(null)
      try {
        setActiveRun(run)
        setConfig({ ...DEFAULT_CONFIG, ...run.config })
        await loadRunAssets(run)
      } catch (err) {
        setError(err instanceof Error ? err.message : String(err))
      } finally {
        setBusy(null)
      }
    },
    [runs, loadRunAssets],
  )

  const uploadStl = useCallback(
    async (file: File) => {
      setBusy('meshing geometry…')
      setError(null)
      try {
        const name = file.name.replace(/\.stl$/i, '') || 'geometry'
        const created = await createRun(file, config, name)
        setActiveRun(created.run)
        setReport(created.geometry_report)
        setStlBuffer(await file.arrayBuffer())
        await refreshRuns()
      } catch (err) {
        setError(err instanceof Error ? err.message : String(err))
        throw err
      } finally {
        setBusy(null)
      }
    },
    [config, refreshRuns],
  )

  const updateConfig = useCallback((patch: Partial<CaseConfig>) => {
    setConfig((current) => ({ ...current, ...patch }))
  }, [])

  const setFluid = useCallback(
    (patch: Partial<FluidState>) => {
      setFluidState((current) => {
        const next = { ...current, ...patch }
        // Re is derived live from the fluid properties: Re = rho * U * h / mu.
        setConfig((cfg) => ({
          ...cfg,
          reynolds: Math.max(1, Math.round(reynolds(next, charLength(report)) * 100) / 100),
        }))
        return next
      })
    },
    [report],
  )

  const setOutputs = useCallback((patch: Partial<OutputsState>) => {
    setOutputsState((current) => ({ ...current, ...patch }))
  }, [])

  const setTransient = useCallback((value: boolean) => {
    setTransientState(value)
    setConfig((cfg) => ({ ...cfg, steady_tol: value ? 0 : 1e-5 }))
  }, [])

  const applyCard = useCallback(
    (card: ExperimentCard) => {
      // typed whitelist (mirrors the backend's card_to_case_config)
      const slug =
        card.title
          .replace(/[^A-Za-z0-9._-]+/g, '-')
          .replace(/^-+|-+$/g, '')
          .slice(0, 64) || 'experiment'
      setConfig((cfg) => ({
        ...cfg,
        type: card.case_type,
        name: slug,
        reynolds: card.reynolds_number,
        resolution: card.resolution,
        max_steps: card.max_steps,
        steady_tol: card.steady ? 1e-5 : 0,
      }))
      setTransientState(!card.steady)
      setOutputsState(outputsFromCard(card.target_outputs))
      setFluidState((current) => {
        const h = charLength(report)
        const uInf = card.inlet_velocity
        // keep rho, derive mu so that Re matches the confirmed card
        const mu = (current.rho * uInf * h) / card.reynolds_number
        return { ...current, uInf, mu }
      })
    },
    [report],
  )

  const launch = useCallback(async (): Promise<RunInfo> => {
    if (!activeRun) throw new Error('no active run')
    setBusy('launching…')
    setError(null)
    try {
      const started = await startRun(activeRun.id, config)
      setActiveRun(started)
      await refreshRuns()
      return started
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err))
      throw err
    } finally {
      setBusy(null)
    }
  }, [activeRun, config, refreshRuns])

  const value = useMemo<RunContextValue>(
    () => ({
      runs,
      activeRun,
      report,
      stlBuffer,
      config,
      fluid,
      outputs,
      transient,
      busy,
      error,
      refreshRuns,
      selectRun,
      uploadStl,
      updateConfig,
      setFluid,
      setOutputs,
      setTransient,
      applyCard,
      launch,
      clearError: () => setError(null),
    }),
    [
      runs,
      activeRun,
      report,
      stlBuffer,
      config,
      fluid,
      outputs,
      transient,
      busy,
      error,
      refreshRuns,
      selectRun,
      uploadStl,
      updateConfig,
      setFluid,
      setOutputs,
      setTransient,
      applyCard,
      launch,
    ],
  )

  return <RunContext.Provider value={value}>{children}</RunContext.Provider>
}

export function useRunContext(): RunContextValue {
  const ctx = useContext(RunContext)
  if (!ctx) throw new Error('useRunContext must be used inside RunProvider')
  return ctx
}
