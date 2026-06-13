// Adaptive Fidelity Map: 2D centerplane slice (y or z selectable) where every
// cell is colored by refinement level or physics-model label. The slice is
// extracted server-side from the latest snapshot — no 3D fields in the browser.

import { useCallback, useEffect, useRef, useState } from 'react'

import { fetchFidelitySlice, type SliceResponse } from '../../api'

const POLL_MS = 2500

const MODE_COLORS: Record<number, string> = {
  0: '#e0455e', // FULL_NS
  1: '#f0883e', // NEAR_WALL
  2: '#e3c53d', // LAMINAR_BL
  3: '#3f7fd6', // INVISCID
  4: '#9d5bd2', // WAKE_SHEAR
  99: '#3a4356', // SOLID
}
const MODE_LABELS: [number, string][] = [
  [0, 'FULL_NS'],
  [1, 'NEAR_WALL'],
  [2, 'LAMINAR_BL'],
  [3, 'INVISCID'],
  [4, 'WAKE_SHEAR'],
  [99, 'solid'],
]
const LEVEL_RAMP = ['#15314f', '#1c4a72', '#1f6695', '#2a85b4', '#46a5c9', '#74c4d8', '#aee0e6']

function levelColor(level: number, minLevel: number, maxLevel: number): string {
  if (maxLevel <= minLevel) return LEVEL_RAMP[3]
  const t = (level - minLevel) / (maxLevel - minLevel)
  return LEVEL_RAMP[Math.min(LEVEL_RAMP.length - 1, Math.round(t * (LEVEL_RAMP.length - 1)))]
}

interface Props {
  runId: string
  live: boolean // poll while the run is active
}

export function FidelityMap({ runId, live }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const [axis, setAxis] = useState<'y' | 'z'>('z')
  const [colorBy, setColorBy] = useState<'level' | 'mode'>('level')
  const [slice, setSlice] = useState<SliceResponse | null>(null)
  const [unavailable, setUnavailable] = useState(false)

  const poll = useCallback(async () => {
    try {
      const next = await fetchFidelitySlice(runId, axis, 0.5)
      setSlice(next)
      setUnavailable(false)
    } catch {
      setUnavailable(true)
    }
  }, [runId, axis])

  useEffect(() => {
    void poll()
    if (!live) return
    const timer = setInterval(() => void poll(), POLL_MS)
    return () => clearInterval(timer)
  }, [poll, live])

  // canvas render
  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas || !slice) return
    const parent = canvas.parentElement
    const cssWidth = parent?.clientWidth ?? 360
    const [x0, x1, y0, y1] = slice.bounds
    const spanX = Math.max(x1 - x0, 1e-9)
    const spanY = Math.max(y1 - y0, 1e-9)
    const cssHeight = Math.max(120, Math.min(420, (cssWidth * spanY) / spanX))
    const dpr = window.devicePixelRatio || 1
    canvas.width = cssWidth * dpr
    canvas.height = cssHeight * dpr
    canvas.style.width = `${cssWidth}px`
    canvas.style.height = `${cssHeight}px`

    const ctx = canvas.getContext('2d')
    if (!ctx) return
    ctx.scale(dpr, dpr)
    ctx.fillStyle = '#0b1322'
    ctx.fillRect(0, 0, cssWidth, cssHeight)

    const levels = slice.cells.map((c) => c.level)
    const minLevel = Math.min(...levels)
    const maxLevel = Math.max(...levels)
    const sx = cssWidth / spanX
    const sy = cssHeight / spanY

    for (const cell of slice.cells) {
      const px = (cell.x - x0) * sx
      // canvas y is down; physical v-axis is up
      const py = cssHeight - (cell.y - y0 + cell.h) * sy
      const pw = Math.max(cell.w * sx - 0.4, 0.6)
      const ph = Math.max(cell.h * sy - 0.4, 0.6)
      if (colorBy === 'mode' && cell.mode !== null) {
        ctx.fillStyle = MODE_COLORS[cell.mode] ?? '#888'
      } else if (cell.mode === 99) {
        ctx.fillStyle = MODE_COLORS[99] // solid stays visible in level view too
      } else {
        ctx.fillStyle = levelColor(cell.level, minLevel, maxLevel)
      }
      ctx.fillRect(px, py, pw, ph)
    }
  }, [slice, colorBy])

  return (
    <div className="sim-panel fidelity">
      <div className="fidelity-head">
        <h4>Adaptive fidelity map</h4>
        <div className="fidelity-controls">
          <select value={axis} onChange={(e) => setAxis(e.target.value as 'y' | 'z')}>
            <option value="z">centerplane z</option>
            <option value="y">centerplane y</option>
          </select>
          <select value={colorBy} onChange={(e) => setColorBy(e.target.value as 'level' | 'mode')}>
            <option value="level">refinement level</option>
            <option value="mode">physics model</option>
          </select>
        </div>
      </div>
      {unavailable && !slice && <p className="muted">no mesh snapshot yet…</p>}
      <canvas ref={canvasRef} />
      <div className="fidelity-legend">
        {colorBy === 'mode' ? (
          MODE_LABELS.map(([mode, label]) => (
            <span key={mode} className="legend-item">
              <i style={{ background: MODE_COLORS[mode] }} /> {label}
            </span>
          ))
        ) : (
          <>
            <span className="legend-item">
              <i style={{ background: LEVEL_RAMP[0] }} /> coarse
            </span>
            <span className="legend-item">
              <i style={{ background: LEVEL_RAMP[LEVEL_RAMP.length - 1] }} /> fine
            </span>
            <span className="legend-item">
              <i style={{ background: MODE_COLORS[99] }} /> solid
            </span>
          </>
        )}
        {slice && (
          <span className="muted">
            {slice.cells.length.toLocaleString()} cells · {slice.source}
          </span>
        )}
      </div>
    </div>
  )
}
