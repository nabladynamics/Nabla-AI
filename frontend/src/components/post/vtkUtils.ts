// VTK.js helpers for the Post-simulation viewers. The scene layer is vtk.js
// (GenericRenderWindow + polydata mappers); geometry (slices, streamlines,
// box surfaces) is built here from the backend's decimated field bundles, so
// the heavy lifting stays simple, typed, and laptop-GPU friendly.

import '@kitware/vtk.js/Rendering/Profiles/Geometry'

import vtkActor from '@kitware/vtk.js/Rendering/Core/Actor'
import vtkColorTransferFunction from '@kitware/vtk.js/Rendering/Core/ColorTransferFunction'
import vtkDataArray from '@kitware/vtk.js/Common/Core/DataArray'
import vtkGenericRenderWindow from '@kitware/vtk.js/Rendering/Misc/GenericRenderWindow'
import vtkMapper from '@kitware/vtk.js/Rendering/Core/Mapper'
import vtkPolyData from '@kitware/vtk.js/Common/DataModel/PolyData'

import type { FieldBundle } from '../../api'

// ---- field decoding -------------------------------------------------------------

export interface Field {
  dims: [number, number, number]
  origin: [number, number, number]
  spacing: [number, number, number]
  arrays: Record<string, Float32Array>
  ranges: Record<string, [number, number]>
}

export function decodeBundle(bundle: FieldBundle): Field {
  const arrays: Record<string, Float32Array> = {}
  for (const [name, b64] of Object.entries(bundle.fields)) {
    const raw = atob(b64)
    const bytes = new Uint8Array(raw.length)
    for (let i = 0; i < raw.length; i += 1) bytes[i] = raw.charCodeAt(i)
    arrays[name] = new Float32Array(bytes.buffer)
  }
  return {
    dims: bundle.dims,
    origin: bundle.origin,
    spacing: bundle.spacing,
    arrays,
    ranges: bundle.ranges,
  }
}

export function fieldIndex(field: Field, i: number, j: number, k: number): number {
  const [nx, ny] = field.dims
  return i + nx * (j + ny * k)
}

/** Trilinear sample of a scalar array at physical (x, y, z); null outside. */
export function sampleField(field: Field, name: string, x: number, y: number, z: number): number | null {
  const arr = field.arrays[name]
  if (!arr) return null
  const [nx, ny, nz] = field.dims
  const fx = (x - field.origin[0]) / field.spacing[0]
  const fy = (y - field.origin[1]) / field.spacing[1]
  const fz = (z - field.origin[2]) / field.spacing[2]
  if (fx < 0 || fy < 0 || fz < 0 || fx > nx - 1 || fy > ny - 1 || fz > nz - 1) return null
  const i0 = Math.min(Math.floor(fx), nx - 2 < 0 ? 0 : nx - 2)
  const j0 = Math.min(Math.floor(fy), ny - 2 < 0 ? 0 : ny - 2)
  const k0 = Math.min(Math.floor(fz), nz - 2 < 0 ? 0 : nz - 2)
  const tx = Math.min(Math.max(fx - i0, 0), 1)
  const ty = Math.min(Math.max(fy - j0, 0), 1)
  const tz = Math.min(Math.max(fz - k0, 0), 1)
  const i1 = Math.min(i0 + 1, nx - 1)
  const j1 = Math.min(j0 + 1, ny - 1)
  const k1 = Math.min(k0 + 1, nz - 1)
  const v = (i: number, j: number, k: number) => arr[fieldIndex(field, i, j, k)]
  const c00 = v(i0, j0, k0) * (1 - tx) + v(i1, j0, k0) * tx
  const c10 = v(i0, j1, k0) * (1 - tx) + v(i1, j1, k0) * tx
  const c01 = v(i0, j0, k1) * (1 - tx) + v(i1, j0, k1) * tx
  const c11 = v(i0, j1, k1) * (1 - tx) + v(i1, j1, k1) * tx
  const c0 = c00 * (1 - ty) + c10 * ty
  const c1 = c01 * (1 - ty) + c11 * ty
  return c0 * (1 - tz) + c1 * tz
}

// ---- viewport ----------------------------------------------------------------------

export interface Viewport {
  addActor: (actor: vtkActor) => void
  removeActor: (actor: vtkActor) => void
  render: () => void
  resetCamera: () => void
  captureImage: () => Promise<string>
  dispose: () => void
}

export function createViewport(container: HTMLElement): Viewport {
  const grw = vtkGenericRenderWindow.newInstance({ background: [0.05, 0.08, 0.14] })
  grw.setContainer(container)
  grw.resize()
  const renderer = grw.getRenderer()
  // Light isosurfaces from both faces so a Q-criterion shell is never fully dark
  // when its normals point away from the auto headlight.
  renderer.setTwoSidedLighting(true)
  const renderWindow = grw.getRenderWindow()
  // A viewport mounted on a freshly-switched tab is created before the browser
  // has laid out its container, so the initial grw.resize() sizes the GL canvas
  // to 0 and the first renders paint nothing. The ResizeObserver must therefore
  // re-render (not just resize) once layout settles, or the canvas stays blank
  // until the user interacts. Re-frame on the first real size so the geometry
  // fits the now-correct aspect.
  let framedOnce = false
  const observer = new ResizeObserver(() => {
    grw.resize()
    if (!framedOnce && container.clientWidth > 0 && container.clientHeight > 0) {
      framedOnce = true
      renderer.resetCamera()
    }
    renderWindow.render()
  })
  observer.observe(container)
  return {
    addActor: (actor) => renderer.addActor(actor),
    removeActor: (actor) => renderer.removeActor(actor),
    render: () => renderWindow.render(),
    resetCamera: () => {
      renderer.resetCamera()
      renderWindow.render()
    },
    captureImage: async () => {
      const images = await Promise.all(renderWindow.captureImages())
      return images[0] ?? ''
    },
    dispose: () => {
      observer.disconnect()
      // Free the WebGL context immediately. Browsers cap simultaneous WebGL
      // contexts (~16) and silently drop the OLDEST when exceeded; with React
      // StrictMode double-mounting and per-tab viewport churn, leaving GC to
      // reclaim contexts lets them pile up and kills later viewports (the
      // "blank 3D viewport after switching tabs" bug). Losing it explicitly and
      // removing the canvas keeps the live-context count at one per viewport.
      const canvas = container.querySelector('canvas')
      grw.delete()
      if (canvas) {
        const gl = canvas.getContext('webgl2') ?? canvas.getContext('webgl')
        gl?.getExtension('WEBGL_lose_context')?.loseContext()
        canvas.remove()
      }
    },
  }
}

// ---- color maps ----------------------------------------------------------------------

const VIRIDIS: [number, number, number, number][] = [
  [0.0, 0.267, 0.005, 0.329],
  [0.25, 0.229, 0.322, 0.546],
  [0.5, 0.128, 0.567, 0.551],
  [0.75, 0.369, 0.789, 0.383],
  [1.0, 0.993, 0.906, 0.144],
]
const DIVERGING: [number, number, number, number][] = [
  [0.0, 0.123, 0.299, 0.66],
  [0.5, 0.95, 0.95, 0.95],
  [1.0, 0.7, 0.04, 0.15],
]

export function makeColorFunction(range: [number, number], scheme: 'viridis' | 'diverging') {
  const ctf = vtkColorTransferFunction.newInstance()
  const stops = scheme === 'viridis' ? VIRIDIS : DIVERGING
  const [lo, hi] = range[0] === range[1] ? [range[0], range[0] + 1] : range
  for (const [t, r, g, b] of stops) ctf.addRGBPoint(lo + t * (hi - lo), r, g, b)
  return ctf
}

// ---- polydata builders ------------------------------------------------------------------

export function scalarPolyDataActor(
  positions: Float32Array,
  polys: Uint32Array | null,
  lines: Uint32Array | null,
  scalars: Float32Array | null,
  options: { range?: [number, number]; scheme?: 'viridis' | 'diverging'; color?: [number, number, number]; opacity?: number },
): vtkActor {
  const pd = vtkPolyData.newInstance()
  pd.getPoints().setData(positions, 3)
  if (polys) pd.getPolys().setData(polys)
  if (lines) pd.getLines().setData(lines)
  const mapper = vtkMapper.newInstance()
  mapper.setInputData(pd)
  if (scalars && options.range) {
    pd.getPointData().setScalars(
      vtkDataArray.newInstance({ name: 'scalars', values: scalars, numberOfComponents: 1 }),
    )
    mapper.setLookupTable(makeColorFunction(options.range, options.scheme ?? 'viridis'))
    mapper.setScalarRange(options.range[0], options.range[1])
  }
  const actor = vtkActor.newInstance()
  actor.setMapper(mapper)
  if (options.color) actor.getProperty().setColor(...options.color)
  if (options.opacity !== undefined) actor.getProperty().setOpacity(options.opacity)
  // Slices/streamlines/box quads carry no surface normals, so diffuse lighting
  // (ambient 0, diffuse 1 by default) would render them black on the dark
  // background — the cause of the "blank 3D viewport". Render them flat so the
  // colormap / solid colour shows at full intensity regardless of light angle.
  actor.getProperty().setLighting(false)
  return actor
}

/** Axis-aligned slice through the cell lattice as independent quads with
 *  per-vertex scalars (4 verts per cell — no shared topology to manage). */
export function buildSliceActor(
  field: Field,
  scalarName: string,
  axis: 0 | 1 | 2,
  sliceIndex: number,
  scheme: 'viridis' | 'diverging' = 'viridis',
): vtkActor {
  const [nx, ny, nz] = field.dims
  const n = [nx, ny, nz]
  const uAxis = axis === 0 ? 1 : 0
  const vAxis = axis === 2 ? 1 : 2
  const scalars = field.arrays[scalarName]
  const solid = field.arrays.solid
  const index = Math.min(Math.max(sliceIndex, 0), n[axis] - 1)

  const quads: number[] = []
  const positions: number[] = []
  const values: number[] = []
  const idx3 = [0, 0, 0]
  for (let b = 0; b < n[vAxis]; b += 1) {
    for (let a = 0; a < n[uAxis]; a += 1) {
      idx3[axis] = index
      idx3[uAxis] = a
      idx3[vAxis] = b
      const c = fieldIndex(field, idx3[0], idx3[1], idx3[2])
      const value = solid && solid[c] > 0.5 ? NaN : scalars[c]
      const center = [
        field.origin[0] + idx3[0] * field.spacing[0],
        field.origin[1] + idx3[1] * field.spacing[1],
        field.origin[2] + idx3[2] * field.spacing[2],
      ]
      const du = [0, 0, 0]
      const dv = [0, 0, 0]
      du[uAxis] = field.spacing[uAxis] / 2
      dv[vAxis] = field.spacing[vAxis] / 2
      const base = positions.length / 3
      for (const [su, sv] of [
        [-1, -1],
        [1, -1],
        [1, 1],
        [-1, 1],
      ]) {
        positions.push(
          center[0] + su * du[0] + sv * dv[0],
          center[1] + su * du[1] + sv * dv[1],
          center[2] + su * du[2] + sv * dv[2],
        )
        values.push(Number.isNaN(value) ? field.ranges[scalarName]?.[0] ?? 0 : value)
      }
      quads.push(4, base, base + 1, base + 2, base + 3)
    }
  }
  return scalarPolyDataActor(
    new Float32Array(positions),
    new Uint32Array(quads),
    null,
    new Float32Array(values),
    { range: field.ranges[scalarName] ?? [0, 1], scheme },
  )
}

/** Box surface (used for the geometry/STL overlay and domain outline). */
export function buildBoxActor(
  bounds: [number, number, number, number, number, number],
  color: [number, number, number],
  opacity: number,
): vtkActor {
  const [x0, x1, y0, y1, z0, z1] = bounds
  const corners = [
    [x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0],
    [x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1],
  ]
  const positions = new Float32Array(corners.flat())
  const polys = new Uint32Array([
    4, 0, 1, 2, 3, 4, 4, 5, 6, 7, 4, 0, 1, 5, 4,
    4, 2, 3, 7, 6, 4, 0, 3, 7, 4, 4, 1, 2, 6, 5,
  ])
  const actor = scalarPolyDataActor(positions, polys, null, null, { color, opacity })
  return actor
}

// ---- streamlines (RK2 over the decimated field; rendered as polylines) ---------------

export function traceStreamlines(
  field: Field,
  seeds: [number, number, number][],
  maxSteps = 600,
): { positions: Float32Array; lines: Uint32Array; speeds: Float32Array } {
  const h = Math.min(...field.spacing) * 0.5
  const positions: number[] = []
  const lines: number[] = []
  const speeds: number[] = []

  const velocity = (x: number, y: number, z: number): [number, number, number] | null => {
    const u = sampleField(field, 'u', x, y, z)
    const v = sampleField(field, 'v', x, y, z)
    const w = sampleField(field, 'w', x, y, z)
    if (u === null || v === null || w === null) return null
    return [u, v, w]
  }

  for (const seed of seeds) {
    let [x, y, z] = seed
    const start = positions.length / 3
    let count = 0
    for (let s = 0; s < maxSteps; s += 1) {
      const vel = velocity(x, y, z)
      if (!vel) break
      const speed = Math.hypot(...vel)
      if (speed < 1e-5) break
      positions.push(x, y, z)
      speeds.push(speed)
      count += 1
      // RK2 midpoint
      const xm = x + (0.5 * h * vel[0]) / speed
      const ym = y + (0.5 * h * vel[1]) / speed
      const zm = z + (0.5 * h * vel[2]) / speed
      const mid = velocity(xm, ym, zm)
      if (!mid) break
      const ms = Math.hypot(...mid)
      if (ms < 1e-5) break
      x += (h * mid[0]) / ms
      y += (h * mid[1]) / ms
      z += (h * mid[2]) / ms
    }
    if (count >= 2) {
      lines.push(count)
      for (let i = 0; i < count; i += 1) lines.push(start + i)
    } else {
      positions.length = start * 3
      speeds.length = start
    }
  }
  return {
    positions: new Float32Array(positions),
    lines: new Uint32Array(lines),
    speeds: new Float32Array(speeds),
  }
}

/** Copy a rendered figure (canvas or data URL) to the clipboard as PNG. */
export async function copyImageToClipboard(source: HTMLCanvasElement | string): Promise<void> {
  const dataUrl = typeof source === 'string' ? source : source.toDataURL('image/png')
  const blob = await (await fetch(dataUrl)).blob()
  await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })])
}
