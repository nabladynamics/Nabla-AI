// Live telemetry over the run WebSocket: ring-buffered series for the charts,
// per-step LOCAL state, humanized audit feed. Reconnects automatically; the
// server replays the full file snapshot on connect, so reconnect = clear
// buffers and let the replay refill them (no gaps, no duplicates).

import { useEffect, useRef, useState } from 'react'

import { wsPath } from '../config'

const RING_CAP = 5000
const FLUSH_MS = 250
const RECONNECT_MS = 1500
const FEED_CAP = 250
const MEAN_WINDOW = 100

export interface GlobalSeries {
  t: number[]
  step: number[]
  dt: number[]
  cfl: number[]
  cd: number[]
  cl: number[]
  cdMean: number[]
  clMean: number[]
  momentum: number[]
  continuity: number[]
  cells: number[]
  poissonIters: number[]
}

export interface FractionPoint {
  step: number
  cheap: number // fraction of cells on reduced (cheap) physics
  full: number // fraction on FULL_NS / DNS-oriented resolution
}

export interface LocalState {
  step: number
  dt: number
  cfl: number
  cd: number
  cl: number
  poissonIters: number
  refined: number
  coarsened: number
  cheapFrac: number | null
  decision: string | null
}

export interface AuditEntry {
  id: number
  step: number
  kind: 'refine' | 'coarsen' | 'promote' | 'accept' | 'guard' | 'decision' | 'info'
  text: string
}

export interface Telemetry {
  series: GlobalSeries
  fractions: FractionPoint[]
  local: LocalState | null
  feed: AuditEntry[]
  status: string | null
  connected: boolean
  eof: boolean
  // Bumped on every telemetry message. The series/fractions arrays are mutated
  // in place (stable object identity), so consumers that memoize chart data
  // must depend on this counter to recompute when new points arrive.
  rev: number
}

function emptySeries(): GlobalSeries {
  return {
    t: [],
    step: [],
    dt: [],
    cfl: [],
    cd: [],
    cl: [],
    cdMean: [],
    clMean: [],
    momentum: [],
    continuity: [],
    cells: [],
    poissonIters: [],
  }
}

function pushCapped(array: number[], value: number): void {
  array.push(value)
  if (array.length > RING_CAP) array.splice(0, array.length - RING_CAP)
}

function trailingMean(values: number[]): number {
  const n = Math.min(values.length, MEAN_WINDOW)
  let sum = 0
  for (let i = values.length - n; i < values.length; i += 1) sum += values[i]
  return n ? sum / n : 0
}

interface DiagLine {
  step: number
  t: number
  dt: number
  cfl: number
  momentum_residual: number
  continuity_residual: number
  cd: number
  cl: number
  poisson_iters: number
  cells: number
}

interface AuditLine {
  event: string
  step?: number
  count?: number
  reason?: string
  cell?: number
  from?: string
  to?: string
  region?: string
  mode?: string
  decision?: string
  detail?: string
  cells?: number
  cd?: number
  cl?: number
}

const ACCEPTED_RE = /(\d+) accepted, (\d+) rejected/
const GUARD_RE = /(\d+) reduced proposals/

export function useTelemetry(runId: string | null): Telemetry {
  const [, setVersion] = useState(0)
  const data = useRef<Telemetry>({
    series: emptySeries(),
    fractions: [],
    local: null,
    feed: [],
    status: null,
    connected: false,
    eof: false,
    rev: 0,
  })
  const feedId = useRef(0)
  const cheapThisStep = useRef(0)
  const refinedThisStep = useRef(0)
  const coarsenedThisStep = useRef(0)

  useEffect(() => {
    if (!runId) return
    let socket: WebSocket | null = null
    let closedByUs = false
    let reconnectTimer: number | undefined

    const reset = () => {
      data.current = {
        series: emptySeries(),
        fractions: [],
        local: null,
        feed: [],
        status: data.current.status,
        connected: false,
        eof: false,
        rev: 0,
      }
      cheapThisStep.current = 0
      refinedThisStep.current = 0
      coarsenedThisStep.current = 0
    }

    const pushFeed = (step: number, kind: AuditEntry['kind'], text: string) => {
      data.current.feed.unshift({ id: (feedId.current += 1), step, kind, text })
      if (data.current.feed.length > FEED_CAP) data.current.feed.length = FEED_CAP
    }

    const onDiagnostics = (line: DiagLine) => {
      if (typeof line.step !== 'number') return // provenance meta line
      const s = data.current.series
      pushCapped(s.t, line.t)
      pushCapped(s.step, line.step)
      pushCapped(s.dt, line.dt)
      pushCapped(s.cfl, line.cfl)
      pushCapped(s.cd, line.cd)
      pushCapped(s.cl, line.cl)
      pushCapped(s.cdMean, trailingMean(s.cd))
      pushCapped(s.clMean, trailingMean(s.cl))
      pushCapped(s.momentum, line.momentum_residual)
      pushCapped(s.continuity, line.continuity_residual)
      pushCapped(s.cells, line.cells)
      pushCapped(s.poissonIters, line.poisson_iters)
      data.current.local = {
        ...(data.current.local ?? {
          refined: 0,
          coarsened: 0,
          cheapFrac: null,
          decision: null,
        }),
        step: line.step,
        dt: line.dt,
        cfl: line.cfl,
        cd: line.cd,
        cl: line.cl,
        poissonIters: line.poisson_iters,
      }
    }

    const onAudit = (line: AuditLine) => {
      const step = line.step ?? 0
      switch (line.event) {
        case 'step_begin':
          cheapThisStep.current = 0
          refinedThisStep.current = 0
          coarsenedThisStep.current = 0
          break
        case 'refine':
          refinedThisStep.current += line.count ?? 0
          pushFeed(step, 'refine', `Refined ${line.count} cells — ${line.reason ?? ''}`)
          break
        case 'coarsen':
          coarsenedThisStep.current += line.count ?? 0
          pushFeed(step, 'coarsen', `Coarsened ${line.count} cells — ${line.reason ?? ''}`)
          break
        case 'model_change': {
          const reason = line.reason ?? ''
          if (line.to === 'FULL_NS' && !reason.startsWith('accepted')) {
            pushFeed(step, 'promote', `Cell ${line.cell} promoted to FULL_NS: ${reason}`)
          }
          break
        }
        case 'acceptance': {
          const reason = line.reason ?? ''
          if (line.region === 'hard-guard-zones') {
            const m = GUARD_RE.exec(reason)
            pushFeed(
              step,
              'guard',
              `Hard guards: ${m ? m[1] : '?'} reduced proposals forced to FULL_NS`,
            )
            break
          }
          const m = ACCEPTED_RE.exec(reason)
          if (m) {
            cheapThisStep.current += Number(m[1])
            const rejected = Number(m[2])
            pushFeed(
              step,
              rejected > 0 ? 'promote' : 'accept',
              `${line.mode}: ${m[1]} accepted${rejected ? `, ${rejected} rejected → FULL_NS` : ''}`,
            )
          }
          break
        }
        case 'metrics': {
          const cells = line.cells ?? 0
          if (cells > 0) {
            const cheap = Math.min(cheapThisStep.current / cells, 1)
            data.current.fractions.push({ step, cheap, full: 1 - cheap })
            if (data.current.fractions.length > RING_CAP) data.current.fractions.shift()
            if (data.current.local) {
              data.current.local.cheapFrac = cheap
              data.current.local.refined = refinedThisStep.current
              data.current.local.coarsened = coarsenedThisStep.current
            }
          }
          break
        }
        case 'step_decision':
          pushFeed(
            step,
            line.decision === 'accept' ? 'decision' : 'promote',
            `Step ${line.decision}${line.detail ? ` — ${line.detail}` : ''}`,
          )
          if (data.current.local) data.current.local.decision = line.decision ?? null
          break
        default:
          break
      }
    }

    const connect = () => {
      reset() // server replays the snapshot from byte 0 on every connect
      socket = new WebSocket(wsPath(`/api/runs/${runId}/telemetry`))
      socket.onopen = () => {
        data.current.connected = true
      }
      socket.onmessage = (message: MessageEvent<string>) => {
        const event = JSON.parse(message.data) as { stream: string; data: unknown }
        if (event.stream === 'diagnostics') onDiagnostics(event.data as DiagLine)
        else if (event.stream === 'audit') onAudit(event.data as AuditLine)
        else if (event.stream === 'status') {
          data.current.status = (event.data as { status: string }).status
        } else if (event.stream === 'eof') {
          data.current.eof = true
        }
        // mark the in-place buffers dirty so memoized chart data recomputes
        data.current.rev += 1
      }
      socket.onclose = () => {
        data.current.connected = false
        if (!closedByUs && !data.current.eof) {
          reconnectTimer = window.setTimeout(connect, RECONNECT_MS)
        }
        setVersion((v) => v + 1)
      }
    }

    connect()
    const flusher = window.setInterval(() => setVersion((v) => v + 1), FLUSH_MS)
    return () => {
      closedByUs = true
      window.clearInterval(flusher)
      window.clearTimeout(reconnectTimer)
      socket?.close()
    }
  }, [runId])

  return data.current
}
