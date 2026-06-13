// Typed client for the orchestration backend (/api/*). The frontend talks
// ONLY to the backend over HTTP — never to the solver core (CLAUDE.md rule 1).

export type RunStatus =
  | 'created'
  | 'meshing'
  | 'running'
  | 'paused'
  | 'completed'
  | 'failed'

export interface CaseConfig {
  type: 'wall-mounted-cube' | 'channel' | 'lid-cavity'
  name: string
  reynolds: number
  resolution: number
  max_steps: number
  snapshot_every: number
  checkpoint_every: number
  cfl: number
  convection: 'weno5' | 'central'
  steady_tol: number
  adaptive: boolean
  adaptive_warmup: number
}

export const DEFAULT_CONFIG: CaseConfig = {
  type: 'wall-mounted-cube',
  name: 'run',
  reynolds: 500,
  resolution: 6,
  max_steps: 240,
  snapshot_every: 20,
  checkpoint_every: 40,
  cfl: 0.7,
  convection: 'weno5',
  steady_tol: 0,
  adaptive: false,
  adaptive_warmup: 25,
}

export interface RunInfo {
  id: string
  name: string
  status: RunStatus
  config: Partial<CaseConfig>
  artifacts: Record<string, string>
  error: string | null
  created_at: string
  updated_at: string
}

export interface GeometryReport {
  case: string
  cleaning: {
    watertight: boolean
    triangles: number
    vertices: number
    boundary_edges?: number
  }
  bounding_box: { min: number[]; max: number[]; size: number[] }
  characteristic_length: number
  smallest_feature?: number
  features: {
    sharp_edges: { count: number; angle_threshold_deg?: number }
    corners: { count: number }
  }
  domain?: { origin: number[]; extent: number[]; cube_height_h: number }
  mesh: {
    cells: number
    inside_solid?: number
    cut?: number
    fluid?: number
    min_level?: number
    max_level?: number
    balanced?: boolean
    surface_target_size?: number
    edge_target_size?: number
  }
}

export type TargetOutput =
  | 'cd'
  | 'cl'
  | 'strouhal'
  | 'reattachment_length'
  | 'velocity_profiles'
  | 'pressure_distribution'

export interface ExperimentCard {
  title: string
  object_identification: string
  case_type: CaseConfig['type']
  reynolds_number: number
  inlet_velocity: number
  inlet_profile: 'uniform' | 'parabolic'
  steady: boolean
  target_outputs: TargetOutput[]
  resolution: number
  max_steps: number
  confidence: 'low' | 'medium' | 'high'
  open_questions: string[]
}

export interface AskResponse {
  reply: string
  experiment_card: ExperimentCard | null
  case_config: CaseConfig | null
}

export interface ChatTurn {
  role: 'user' | 'assistant'
  content: string
}

class ApiError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message)
  }
}

async function check<T>(response: Response): Promise<T> {
  if (!response.ok) {
    let detail = response.statusText
    try {
      const body: unknown = await response.json()
      if (body && typeof body === 'object' && 'detail' in body) {
        const d = (body as { detail: unknown }).detail
        detail = typeof d === 'string' ? d : JSON.stringify(d)
      }
    } catch {
      /* keep statusText */
    }
    throw new ApiError(response.status, detail)
  }
  return (await response.json()) as T
}

export async function listRuns(): Promise<RunInfo[]> {
  return check(await fetch('/api/runs'))
}

export async function getRun(id: string): Promise<RunInfo> {
  return check(await fetch(`/api/runs/${id}`))
}

export async function createRun(
  stl: File,
  config: CaseConfig,
  name: string,
): Promise<{ run: RunInfo; geometry_report: GeometryReport }> {
  const form = new FormData()
  form.append('stl', stl)
  form.append('config', JSON.stringify(config))
  form.append('name', name)
  return check(await fetch('/api/runs', { method: 'POST', body: form }))
}

export async function startRun(id: string, config?: CaseConfig): Promise<RunInfo> {
  return check(
    await fetch(`/api/runs/${id}/start`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(config ? { config } : {}),
    }),
  )
}

export async function askCopilot(
  id: string,
  message: string,
  history: ChatTurn[],
): Promise<AskResponse> {
  return check(
    await fetch(`/api/runs/${id}/ai/ask`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ message, history }),
    }),
  )
}

export async function pauseRun(id: string): Promise<RunInfo> {
  return check(await fetch(`/api/runs/${id}/pause`, { method: 'POST' }))
}

export async function resumeRun(id: string): Promise<RunInfo> {
  return check(await fetch(`/api/runs/${id}/resume`, { method: 'POST' }))
}

export interface SliceCell {
  x: number
  y: number
  w: number
  h: number
  level: number
  mode: number | null
}

export interface SliceResponse {
  axis: 'y' | 'z'
  coord: number
  bounds: [number, number, number, number]
  source: string
  updated: number
  cells: SliceCell[]
  mode_counts: Record<string, number>
}

export async function fetchFidelitySlice(
  runId: string,
  axis: 'y' | 'z',
  frac: number,
): Promise<SliceResponse> {
  return check(await fetch(`/api/runs/${runId}/fidelity-slice?axis=${axis}&frac=${frac}`))
}

// ---- post-simulation -----------------------------------------------------

export interface ArtifactInfo {
  name: string
  size: number
}

export interface FieldBundle {
  step: number
  source: string
  dims: [number, number, number]
  origin: [number, number, number]
  spacing: [number, number, number]
  stride: number
  fields: Record<string, string> // base64 Float32
  ranges: Record<string, [number, number]>
}

export interface AnalysisResponse {
  window_frac: number
  forces: {
    samples: number
    t0: number
    t1: number
    cd_mean: number
    cd_std: number
    cl_mean: number
    cl_std: number
    cd_min: number
    cd_max: number
    cl_min: number
    cl_max: number
  } | null
  spectrum: {
    st: number[]
    mag: number[]
    strouhal: number | null
    frequency: number | null
    prominent: boolean
    resolution_st: number
    snr: number
    cycles: number
    min_cycles: number
    window_too_short: boolean
    reason: string
  } | null
  recirculation: {
    x_separation: number | null
    x_reattachment: number | null
    upstream_separation_over_h: number | null
    reattachment_over_h: number | null
    reverse_flow_volume_over_h3: number | null
    critical_points: { x: number; kind: string }[]
    cores: { label: string; x: number; y: number; z: number; q: number }[]
  } | null
  comparison: {
    metric: string
    computed: number | null
    reference: number | null
    rel_error: number | null
    band: [number, number]
    status: string
    source: string
  }[]
  audit: {
    accepts: number
    rejects: number
    promotions: number
    guard_overrides: number
    refinements: number
    coarsenings: number
    cell_speedup: number | null
    adaptive_mean_cells: number | null
    uniform_fine_cells: number | null
  } | null
  warnings: string[]
  reference_re: number | null
}

export async function listSnapshots(
  runId: string,
): Promise<{ steps: number[]; has_final: boolean }> {
  return check(await fetch(`/api/runs/${runId}/snapshots`))
}

export async function fetchField(runId: string, step: number, stride = 0): Promise<FieldBundle> {
  return check(await fetch(`/api/runs/${runId}/field?step=${step}&stride=${stride}`))
}

export async function fetchAnalysis(runId: string, window = 0.5): Promise<AnalysisResponse> {
  return check(await fetch(`/api/runs/${runId}/analysis?window=${window}`))
}

export async function generateReport(runId: string): Promise<{ artifact: string }> {
  return check(await fetch(`/api/runs/${runId}/report`, { method: 'POST' }))
}

export function forcesCsvUrl(runId: string): string {
  return `/api/runs/${runId}/forces.csv`
}

export function bundleZipUrl(runId: string, suffix = '.vtu'): string {
  return `/api/runs/${runId}/bundle.zip?suffix=${suffix}`
}

export function artifactUrl(runId: string, name: string): string {
  return `/api/runs/${runId}/artifacts/${name}`
}

export async function fetchArtifactJson<T>(runId: string, name: string): Promise<T> {
  return check(await fetch(artifactUrl(runId, name)))
}

export async function fetchArtifactBuffer(runId: string, name: string): Promise<ArrayBuffer> {
  const response = await fetch(artifactUrl(runId, name))
  if (!response.ok) throw new ApiError(response.status, response.statusText)
  return response.arrayBuffer()
}

export { ApiError }
