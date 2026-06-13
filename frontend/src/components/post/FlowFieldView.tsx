// Flow Field sub-view: VTK.js viewport over backend-decimated snapshots.
// Scalar selector, slice planes, rake-seeded streamlines, point probe,
// geometry overlay, snapshot-time scrubber.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import type vtkActor from '@kitware/vtk.js/Rendering/Core/Actor'

import {
  buildBoxActor,
  buildSliceActor,
  copyImageToClipboard,
  createViewport,
  sampleField,
  scalarPolyDataActor,
  traceStreamlines,
  type Field,
  type Viewport,
} from './vtkUtils'
import type { GeometryReport } from '../../api'

const SCALARS: { key: string; label: string; scheme: 'viridis' | 'diverging' }[] = [
  { key: 'speed', label: 'velocity magnitude', scheme: 'viridis' },
  { key: 'p', label: 'pressure', scheme: 'diverging' },
  { key: 'ke', label: 'kinetic energy (TKE slot)', scheme: 'viridis' },
]

function cubeBounds(report: GeometryReport | null): [number, number, number, number, number, number] | null {
  if (!report) return null
  const size = report.bounding_box.size
  const h = size[1] || 1
  // wall-mounted-cube placement: front face at 3h, on floor, span center 3.2h
  return [3 * h, 3 * h + size[0], 0, size[1], 3.2 * h - size[2] / 2, 3.2 * h + size[2] / 2]
}

interface Props {
  field: Field | null
  steps: number[]
  currentStep: number
  onStepChange: (step: number) => void
  report: GeometryReport | null
  loading?: boolean
  error?: string | null
}

export function FlowFieldView({
  field,
  steps,
  currentStep,
  onStepChange,
  report,
  loading,
  error,
}: Props) {
  const hostRef = useRef<HTMLDivElement>(null)
  const viewportRef = useRef<Viewport | null>(null)
  const actorsRef = useRef<vtkActor[]>([])

  const [scalar, setScalar] = useState('speed')
  const [axis, setAxis] = useState<0 | 1 | 2>(2)
  const [sliceFrac, setSliceFrac] = useState(0.5)
  const [showStreamlines, setShowStreamlines] = useState(true)
  const [rakeY, setRakeY] = useState(0.5)
  const [rakeZ, setRakeZ] = useState(0.5)
  const [showGeometry, setShowGeometry] = useState(true)
  const [probe, setProbe] = useState({ x: 3.5, y: 0.5, z: 3.2 })

  useEffect(() => {
    const host = hostRef.current
    if (!host) return
    const viewport = createViewport(host)
    viewportRef.current = viewport
    return () => {
      viewport.dispose()
      viewportRef.current = null
    }
  }, [])

  const rebuild = useCallback(() => {
    const viewport = viewportRef.current
    if (!viewport || !field) return
    for (const actor of actorsRef.current) viewport.removeActor(actor)
    actorsRef.current = []

    const dims = field.dims
    const sliceIndex = Math.round(sliceFrac * (dims[axis] - 1))
    const scheme = SCALARS.find((s) => s.key === scalar)?.scheme ?? 'viridis'
    actorsRef.current.push(buildSliceActor(field, scalar, axis, sliceIndex, scheme))

    if (showStreamlines) {
      const [x0, y0, z0] = field.origin
      const Ly = field.spacing[1] * (dims[1] - 1)
      const Lz = field.spacing[2] * (dims[2] - 1)
      const seeds: [number, number, number][] = []
      for (let s = 0; s < 14; s += 1) {
        seeds.push([
          x0 + 2 * field.spacing[0],
          y0 + Math.min(Math.max(rakeY + (s / 13 - 0.5) * 0.9, 0.02), 0.98) * Ly,
          z0 + rakeZ * Lz,
        ])
      }
      const { positions, lines, speeds } = traceStreamlines(field, seeds)
      if (lines.length > 0) {
        actorsRef.current.push(
          scalarPolyDataActor(positions, null, lines, speeds, {
            range: field.ranges.speed ?? [0, 1.5],
            scheme: 'viridis',
          }),
        )
      }
    }

    const cube = cubeBounds(report)
    if (showGeometry && cube) {
      actorsRef.current.push(buildBoxActor(cube, [0.55, 0.57, 0.62], 1.0))
    }
    for (const actor of actorsRef.current) viewport.addActor(actor)
    viewport.render()
  }, [field, scalar, axis, sliceFrac, showStreamlines, rakeY, rakeZ, showGeometry, report])

  useEffect(() => {
    rebuild()
  }, [rebuild])

  // first field load: frame the camera
  const framed = useRef(false)
  useEffect(() => {
    if (field && !framed.current && viewportRef.current) {
      viewportRef.current.resetCamera()
      framed.current = true
    }
  }, [field])

  const probeValues = useMemo(() => {
    if (!field) return null
    return {
      u: sampleField(field, 'u', probe.x, probe.y, probe.z),
      v: sampleField(field, 'v', probe.x, probe.y, probe.z),
      w: sampleField(field, 'w', probe.x, probe.y, probe.z),
      p: sampleField(field, 'p', probe.x, probe.y, probe.z),
      speed: sampleField(field, 'speed', probe.x, probe.y, probe.z),
    }
  }, [field, probe])

  const axisName = ['x', 'y', 'z'][axis]
  return (
    <div className="post-view">
      <div className="post-toolbar">
        <select value={scalar} onChange={(e) => setScalar(e.target.value)}>
          {SCALARS.map((s) => (
            <option key={s.key} value={s.key}>
              {s.label}
            </option>
          ))}
        </select>
        <select value={axis} onChange={(e) => setAxis(Number(e.target.value) as 0 | 1 | 2)}>
          <option value={0}>slice ⊥ x</option>
          <option value={1}>slice ⊥ y</option>
          <option value={2}>slice ⊥ z</option>
        </select>
        <label className="slider-label">
          {axisName} pos
          <input
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={sliceFrac}
            onChange={(e) => setSliceFrac(Number(e.target.value))}
          />
        </label>
        <label className="check">
          <input
            type="checkbox"
            checked={showStreamlines}
            onChange={(e) => setShowStreamlines(e.target.checked)}
          />
          streamlines
        </label>
        {showStreamlines && (
          <>
            <label className="slider-label">
              rake y
              <input type="range" min={0.05} max={0.95} step={0.01} value={rakeY} onChange={(e) => setRakeY(Number(e.target.value))} />
            </label>
            <label className="slider-label">
              rake z
              <input type="range" min={0.05} max={0.95} step={0.01} value={rakeZ} onChange={(e) => setRakeZ(Number(e.target.value))} />
            </label>
          </>
        )}
        <label className="check">
          <input type="checkbox" checked={showGeometry} onChange={(e) => setShowGeometry(e.target.checked)} />
          geometry
        </label>
        <button type="button" className="chip-btn" onClick={() => viewportRef.current?.resetCamera()}>
          reset view
        </button>
        <button
          type="button"
          className="chip-btn"
          onClick={() => {
            void viewportRef.current?.captureImage().then((img) => copyImageToClipboard(img))
          }}
        >
          copy figure
        </button>
      </div>

      <div className="post-canvas" ref={hostRef}>
        {loading && <p className="muted post-empty">loading snapshot…</p>}
        {!loading && error && (
          <p className="muted post-empty post-error">could not load snapshot: {error}</p>
        )}
        {!loading && !error && !field && (
          <p className="muted post-empty">no field loaded — run must have snapshots</p>
        )}
      </div>

      <div className="post-footer">
        <label className="slider-label scrubber">
          snapshot
          <input
            type="range"
            min={0}
            max={steps.length}
            step={1}
            value={currentStep < 0 ? steps.length : steps.indexOf(currentStep)}
            onChange={(e) => {
              const index = Number(e.target.value)
              onStepChange(index >= steps.length ? -1 : steps[index])
            }}
          />
          <span>{currentStep < 0 ? 'final' : `step ${currentStep}`}</span>
        </label>
        <div className="probe">
          probe&nbsp;
          {(['x', 'y', 'z'] as const).map((axisKey) => (
            <input
              key={axisKey}
              type="number"
              step="0.1"
              value={probe[axisKey]}
              onChange={(e) => setProbe({ ...probe, [axisKey]: Number(e.target.value) })}
            />
          ))}
          {probeValues && (
            <span className="probe-out">
              u={probeValues.u?.toFixed(3) ?? '—'} v={probeValues.v?.toFixed(3) ?? '—'} p=
              {probeValues.p?.toFixed(3) ?? '—'} |u|={probeValues.speed?.toFixed(3) ?? '—'}
            </span>
          )}
        </div>
      </div>
    </div>
  )
}
