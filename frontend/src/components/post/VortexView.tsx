// Vortex Structures sub-view: Q-criterion isosurface (marching cubes over the
// backend-computed Q field) with a threshold slider, vorticity-magnitude
// slices, and markers for extracted critical points and vortex cores — the
// horseshoe and arch vortices show up as the Q-surface wrapping the cube base
// and closing behind it.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import vtkActor from '@kitware/vtk.js/Rendering/Core/Actor'
import vtkDataArray from '@kitware/vtk.js/Common/Core/DataArray'
import vtkImageData from '@kitware/vtk.js/Common/DataModel/ImageData'
import vtkImageMarchingCubes from '@kitware/vtk.js/Filters/General/ImageMarchingCubes'
import vtkMapper from '@kitware/vtk.js/Rendering/Core/Mapper'

import type { AnalysisResponse, GeometryReport } from '../../api'
import {
  buildBoxActor,
  buildSliceActor,
  copyImageToClipboard,
  createViewport,
  type Field,
  type Viewport,
} from './vtkUtils'

// Q-criterion isosurface. Returns the actor and the number of surface points it
// produced, so the caller can show an explicit empty-state when nothing exceeds
// the threshold (rather than a silently blank canvas).
function qIsoActor(field: Field, threshold: number): { actor: vtkActor; points: number } | null {
  const q = field.arrays.q
  if (!q) return null
  const image = vtkImageData.newInstance()
  image.setDimensions(field.dims[0], field.dims[1], field.dims[2])
  image.setOrigin([...field.origin])
  image.setSpacing([...field.spacing])
  image.getPointData().setScalars(
    vtkDataArray.newInstance({ name: 'q', values: q, numberOfComponents: 1 }),
  )
  const mc = vtkImageMarchingCubes.newInstance({
    contourValue: threshold,
    computeNormals: true,
    mergePoints: true,
  })
  mc.setInputData(image)
  const out = mc.getOutputData()
  const points = out.getPoints().getNumberOfPoints()
  const mapper = vtkMapper.newInstance()
  // Colour by the actor's solid colour, not by any scalar array marching cubes
  // may attach (a degenerate contour-value range would render the shell invisible).
  mapper.setScalarVisibility(false)
  mapper.setInputData(out)
  const actor = vtkActor.newInstance()
  actor.setMapper(mapper)
  actor.getProperty().setColor(0.95, 0.55, 0.2)
  actor.getProperty().setOpacity(0.85)
  // Ambient floor so the isosurface stays visible from every angle (diffuse-only
  // shading with ambient 0 renders the back faces black on the dark background).
  actor.getProperty().setAmbient(0.4)
  actor.getProperty().setDiffuse(0.6)
  return { actor, points }
}

// Quantile of the strictly-positive Q values in the field (Q-criterion is only
// meaningful where rotation dominates strain, i.e. Q > 0). Returns null if the
// field has no positive Q (an irrotational / steady field).
function positiveQuantile(field: Field, p: number): { value: number; count: number } | null {
  const q = field.arrays.q
  const solid = field.arrays.solid
  if (!q) return null
  const pos: number[] = []
  for (let i = 0; i < q.length; i += 1) {
    if (q[i] > 0 && !(solid && solid[i] > 0.5)) pos.push(q[i])
  }
  if (pos.length === 0) return null
  pos.sort((a, b) => a - b)
  const idx = Math.min(pos.length - 1, Math.max(0, Math.round(p * (pos.length - 1))))
  return { value: pos[idx], count: pos.length }
}

function markerActor(x: number, y: number, z: number, size: number, color: [number, number, number]): vtkActor {
  return buildBoxActor([x - size, x + size, y - size, y + size, z - size, z + size], color, 1.0)
}

interface Props {
  field: Field | null
  analysis: AnalysisResponse | null
  report: GeometryReport | null
  loading?: boolean
  error?: string | null
}

export function VortexView({ field, analysis, report, loading, error }: Props) {
  const hostRef = useRef<HTMLDivElement>(null)
  const viewportRef = useRef<Viewport | null>(null)
  const actorsRef = useRef<vtkActor[]>([])
  // Threshold is a percentile of the field's POSITIVE Q (auto-scales to the
  // actual range each snapshot), defaulting to the 90th percentile so the
  // strongest coherent structures show without a magic absolute constant.
  const [percentile, setPercentile] = useState(0.9)
  const [showVortSlice, setShowVortSlice] = useState(false)
  const [vortFrac, setVortFrac] = useState(0.5)
  const [showMarkers, setShowMarkers] = useState(true)
  const [emptyState, setEmptyState] = useState<string | null>(null)

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

  const qInfo = useMemo(() => (field ? positiveQuantile(field, percentile) : null), [field, percentile])
  const threshold = qInfo?.value ?? 0

  const rebuild = useCallback(() => {
    const viewport = viewportRef.current
    if (!viewport || !field) return
    for (const actor of actorsRef.current) viewport.removeActor(actor)
    actorsRef.current = []

    if (!qInfo) {
      setEmptyState('no positive-Q structures in this snapshot — the field is irrotational/steady here')
    } else {
      const iso = qIsoActor(field, threshold)
      if (iso && iso.points > 0) {
        actorsRef.current.push(iso.actor)
        setEmptyState(null)
      } else {
        setEmptyState('no positive-Q structures above the threshold — lower the percentile slider')
      }
    }
    if (showVortSlice) {
      const index = Math.round(vortFrac * (field.dims[2] - 1))
      actorsRef.current.push(buildSliceActor(field, 'vort', 2, index, 'viridis'))
    }
    if (report) {
      const size = report.bounding_box.size
      const h = size[1] || 1
      actorsRef.current.push(
        buildBoxActor(
          [3 * h, 3 * h + size[0], 0, size[1], 3.2 * h - size[2] / 2, 3.2 * h + size[2] / 2],
          [0.55, 0.57, 0.62],
          1.0,
        ),
      )
    }
    if (showMarkers && analysis?.recirculation) {
      const rec = analysis.recirculation
      const h = report?.bounding_box.size[1] ?? 1
      const zc = 3.2 * h
      for (const core of rec.cores) {
        actorsRef.current.push(markerActor(core.x, core.y, core.z, 0.08 * h, [1.0, 0.2, 0.8]))
      }
      // separation / attachment lines drawn as thin spanwise bars on the floor
      const Lz = 1.6 * h
      if (rec.x_separation !== null) {
        actorsRef.current.push(
          buildBoxActor(
            [rec.x_separation - 0.02, rec.x_separation + 0.02, 0, 0.02, zc - Lz, zc + Lz],
            [0.2, 0.9, 0.4],
            1.0,
          ),
        )
      }
      if (rec.x_reattachment !== null) {
        actorsRef.current.push(
          buildBoxActor(
            [rec.x_reattachment - 0.02, rec.x_reattachment + 0.02, 0, 0.02, zc - Lz, zc + Lz],
            [0.3, 0.6, 1.0],
            1.0,
          ),
        )
      }
    }
    for (const actor of actorsRef.current) viewport.addActor(actor)
    viewport.render()
  }, [field, threshold, qInfo, showVortSlice, vortFrac, showMarkers, analysis, report])

  useEffect(() => {
    rebuild()
  }, [rebuild])

  const framed = useRef(false)
  useEffect(() => {
    if (field && !framed.current && viewportRef.current) {
      viewportRef.current.resetCamera()
      framed.current = true
    }
  }, [field])

  const cores = analysis?.recirculation?.cores ?? []
  return (
    <div className="post-view">
      <div className="post-toolbar">
        <label className="slider-label">
          Q percentile
          <input
            type="range"
            min={0.5}
            max={0.99}
            step={0.01}
            value={percentile}
            onChange={(e) => setPercentile(Number(e.target.value))}
          />
          <span>
            p{(percentile * 100).toFixed(0)} · Q={threshold.toExponential(2)}
          </span>
        </label>
        <label className="check">
          <input type="checkbox" checked={showVortSlice} onChange={(e) => setShowVortSlice(e.target.checked)} />
          |ω| slice
        </label>
        {showVortSlice && (
          <label className="slider-label">
            z pos
            <input type="range" min={0} max={1} step={0.01} value={vortFrac} onChange={(e) => setVortFrac(Number(e.target.value))} />
          </label>
        )}
        <label className="check">
          <input type="checkbox" checked={showMarkers} onChange={(e) => setShowMarkers(e.target.checked)} />
          critical points
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
        {!loading && !error && !field && <p className="muted post-empty">no field loaded</p>}
        {!loading && !error && field && emptyState && (
          <p className="muted post-empty">{emptyState}</p>
        )}
      </div>
      <div className="post-footer vortex-legend">
        <span className="legend-item"><i style={{ background: '#f28c33' }} /> Q-criterion isosurface</span>
        <span className="legend-item"><i style={{ background: '#ff33cc' }} /> vortex core (estimated)</span>
        <span className="legend-item"><i style={{ background: '#33e066' }} /> separation line</span>
        <span className="legend-item"><i style={{ background: '#4d99ff' }} /> reattachment line</span>
        {cores.map((core) => (
          <span key={core.label} className="muted">
            {core.label}: ({core.x.toFixed(2)}, {core.y.toFixed(2)}, {core.z.toFixed(2)})
          </span>
        ))}
      </div>
    </div>
  )
}
