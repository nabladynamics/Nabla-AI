// Three.js viewport: renders the uploaded STL (orbit controls + wireframe
// toggle) and, once the domain is built, the domain box with color-coded
// boundaries — inlet green with velocity-profile arrows, walls red, outlet
// blue, fluid translucent — matching the hand-drawn layout.

import { useEffect, useRef, useState } from 'react'
import * as THREE from 'three'
import { OrbitControls } from 'three/addons/controls/OrbitControls.js'
import { STLLoader } from 'three/addons/loaders/STLLoader.js'

import type { GeometryReport } from '../api'

const COLORS = {
  inlet: 0x16a34a, // green
  outlet: 0x2563eb, // blue
  wall: 0xdc2626, // red
  fluid: 0x93c5fd, // translucent blue-gray
  body: 0x64748b,
  edge: 0x0f172a,
}

interface Props {
  stlBuffer: ArrayBuffer | null
  report: GeometryReport | null
  inletProfile: 'uniform' | 'parabolic'
}

interface SceneRefs {
  renderer: THREE.WebGLRenderer
  scene: THREE.Scene
  camera: THREE.PerspectiveCamera
  controls: OrbitControls
  bodyGroup: THREE.Group
  domainGroup: THREE.Group
}

function disposeGroup(group: THREE.Group): void {
  group.traverse((object) => {
    if (object instanceof THREE.Mesh || object instanceof THREE.LineSegments) {
      object.geometry.dispose()
      const material = object.material
      if (Array.isArray(material)) material.forEach((m) => m.dispose())
      else material.dispose()
    }
  })
  group.clear()
}

function boundaryPlane(
  width: number,
  height: number,
  color: number,
  opacity: number,
): THREE.Mesh {
  const material = new THREE.MeshBasicMaterial({
    color,
    transparent: true,
    opacity,
    side: THREE.DoubleSide,
    depthWrite: false,
  })
  return new THREE.Mesh(new THREE.PlaneGeometry(width, height), material)
}

/** Inlet velocity arrows on the x=0 face (grid of ArrowHelpers pointing +x). */
function buildInletArrows(
  Ly: number,
  Lz: number,
  Lx: number,
  profile: 'uniform' | 'parabolic',
): THREE.Group {
  const group = new THREE.Group()
  const rows = 4
  const cols = 5
  const base = 0.085 * Lx
  for (let j = 0; j < rows; j += 1) {
    for (let k = 0; k < cols; k += 1) {
      const y = ((j + 0.5) / rows) * Ly
      const z = ((k + 0.5) / cols) * Lz
      const yn = y / Ly
      const scale = profile === 'parabolic' ? 4 * yn * (1 - yn) * 1.5 : 1
      if (scale < 0.05) continue
      const length = base * scale
      const arrow = new THREE.ArrowHelper(
        new THREE.Vector3(1, 0, 0),
        new THREE.Vector3(0.01 * Lx, y, z),
        length,
        COLORS.inlet,
        0.3 * length,
        0.18 * length,
      )
      group.add(arrow)
    }
  }
  return group
}

function buildDomain(report: GeometryReport, profile: 'uniform' | 'parabolic'): THREE.Group {
  const group = new THREE.Group()
  const extent = report.domain?.extent ?? [14, 3, 6.4]
  const origin = report.domain?.origin ?? [0, 0, 0]
  const [Lx, Ly, Lz] = extent

  // translucent fluid volume
  const fluid = new THREE.Mesh(
    new THREE.BoxGeometry(Lx, Ly, Lz),
    new THREE.MeshBasicMaterial({
      color: COLORS.fluid,
      transparent: true,
      opacity: 0.05,
      depthWrite: false,
    }),
  )
  fluid.position.set(Lx / 2, Ly / 2, Lz / 2)
  group.add(fluid)

  const edges = new THREE.LineSegments(
    new THREE.EdgesGeometry(new THREE.BoxGeometry(Lx, Ly, Lz)),
    new THREE.LineBasicMaterial({ color: 0x94a3b8 }),
  )
  edges.position.copy(fluid.position)
  group.add(edges)

  // inlet (x = 0, green) + velocity-profile arrows
  const inlet = boundaryPlane(Lz, Ly, COLORS.inlet, 0.18)
  inlet.rotation.y = Math.PI / 2
  inlet.position.set(0, Ly / 2, Lz / 2)
  group.add(inlet)
  group.add(buildInletArrows(Ly, Lz, Lx, profile))

  // outlet (x = Lx, blue)
  const outlet = boundaryPlane(Lz, Ly, COLORS.outlet, 0.16)
  outlet.rotation.y = Math.PI / 2
  outlet.position.set(Lx, Ly / 2, Lz / 2)
  group.add(outlet)

  // walls: floor (y = 0) and ceiling (y = Ly), red. Spanwise faces are
  // periodic and stay as the translucent fluid box.
  const floor = boundaryPlane(Lx, Lz, COLORS.wall, 0.14)
  floor.rotation.x = Math.PI / 2
  floor.position.set(Lx / 2, 0.001, Lz / 2)
  group.add(floor)
  const ceiling = boundaryPlane(Lx, Lz, COLORS.wall, 0.1)
  ceiling.rotation.x = Math.PI / 2
  ceiling.position.set(Lx / 2, Ly - 0.001, Lz / 2)
  group.add(ceiling)

  group.position.set(origin[0], origin[1], origin[2])
  return group
}

/** Where the case places the body inside the domain (wall-mounted-cube rule:
 *  front face 3h from the inlet, on the floor, centered in span at 3.2h). */
function bodyTranslation(report: GeometryReport): THREE.Vector3 {
  const bbox = report.bounding_box
  const h = bbox.size[1] || 1
  if (report.case === 'wall-mounted-cube') {
    return new THREE.Vector3(
      3 * h - bbox.min[0],
      0 - bbox.min[1],
      3.2 * h - 0.5 * bbox.size[2] - bbox.min[2],
    )
  }
  return new THREE.Vector3(0, 0, 0)
}

export function Viewport3D({ stlBuffer, report, inletProfile }: Props) {
  const containerRef = useRef<HTMLDivElement>(null)
  const refs = useRef<SceneRefs | null>(null)
  const [wireframe, setWireframe] = useState(false)

  // one-time scene setup
  useEffect(() => {
    const container = containerRef.current
    if (!container) return
    const renderer = new THREE.WebGLRenderer({ antialias: true })
    renderer.setPixelRatio(window.devicePixelRatio)
    renderer.setClearColor(0xf8fafc)
    container.appendChild(renderer.domElement)

    const scene = new THREE.Scene()
    const camera = new THREE.PerspectiveCamera(45, 1, 0.01, 500)
    camera.position.set(14, 8, 14)
    const controls = new OrbitControls(camera, renderer.domElement)
    controls.enableDamping = true

    scene.add(new THREE.HemisphereLight(0xffffff, 0xc7d2da, 1.1))
    const sun = new THREE.DirectionalLight(0xffffff, 1.4)
    sun.position.set(10, 18, 8)
    scene.add(sun)
    const grid = new THREE.GridHelper(40, 40, 0xcbd5e1, 0xe7edf3)
    grid.position.y = -0.002
    scene.add(grid)

    const bodyGroup = new THREE.Group()
    const domainGroup = new THREE.Group()
    scene.add(bodyGroup, domainGroup)

    refs.current = { renderer, scene, camera, controls, bodyGroup, domainGroup }

    const resize = () => {
      const { clientWidth, clientHeight } = container
      renderer.setSize(clientWidth, clientHeight)
      camera.aspect = clientWidth / Math.max(clientHeight, 1)
      camera.updateProjectionMatrix()
    }
    resize()
    const observer = new ResizeObserver(resize)
    observer.observe(container)

    let frame = 0
    const animate = () => {
      frame = requestAnimationFrame(animate)
      controls.update()
      renderer.render(scene, camera)
    }
    animate()

    return () => {
      cancelAnimationFrame(frame)
      observer.disconnect()
      controls.dispose()
      disposeGroup(bodyGroup)
      disposeGroup(domainGroup)
      renderer.dispose()
      container.removeChild(renderer.domElement)
      refs.current = null
    }
  }, [])

  // STL body (re)build
  useEffect(() => {
    const current = refs.current
    if (!current) return
    disposeGroup(current.bodyGroup)
    if (!stlBuffer) return

    const geometry = new STLLoader().parse(stlBuffer)
    geometry.computeVertexNormals()
    const mesh = new THREE.Mesh(
      geometry,
      new THREE.MeshStandardMaterial({
        color: COLORS.body,
        metalness: 0.1,
        roughness: 0.65,
        flatShading: true,
      }),
    )
    const wires = new THREE.LineSegments(
      new THREE.EdgesGeometry(geometry, 20),
      new THREE.LineBasicMaterial({ color: COLORS.edge }),
    )
    wires.visible = wireframe
    wires.name = 'wireframe'
    current.bodyGroup.add(mesh, wires)

    if (report) {
      const shift = bodyTranslation(report)
      current.bodyGroup.position.copy(shift)
    } else {
      current.bodyGroup.position.set(0, 0, 0)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps -- wireframe handled below
  }, [stlBuffer, report])

  // domain overlay (re)build + camera framing
  useEffect(() => {
    const current = refs.current
    if (!current) return
    disposeGroup(current.domainGroup)
    let center = new THREE.Vector3(0.5, 0.5, 0.5)
    let radius = 2
    if (report) {
      current.domainGroup.add(buildDomain(report, inletProfile))
      const extent = report.domain?.extent ?? [14, 3, 6.4]
      center = new THREE.Vector3(extent[0] / 2, extent[1] / 2, extent[2] / 2)
      radius = Math.max(...extent)
    } else if (stlBuffer) {
      radius = 2
      center = new THREE.Vector3(0.5, 0.5, 0.5)
    }
    current.controls.target.copy(center)
    current.camera.position.set(
      center.x + 0.9 * radius,
      center.y + 0.55 * radius,
      center.z + 0.9 * radius,
    )
    current.controls.update()
  }, [report, inletProfile, stlBuffer])

  // wireframe toggle
  useEffect(() => {
    const current = refs.current
    if (!current) return
    const wires = current.bodyGroup.getObjectByName('wireframe')
    if (wires) wires.visible = wireframe
  }, [wireframe])

  return (
    <div className="viewport" ref={containerRef}>
      <div className="viewport-toolbar">
        <button
          type="button"
          className={`chip-btn ${wireframe ? 'chip-btn--on' : ''}`}
          onClick={() => setWireframe((w) => !w)}
        >
          Wireframe
        </button>
      </div>
      {report && (
        <div className="viewport-legend">
          <span className="legend-item">
            <i style={{ background: '#16a34a' }} /> inlet
          </span>
          <span className="legend-item">
            <i style={{ background: '#dc2626' }} /> walls
          </span>
          <span className="legend-item">
            <i style={{ background: '#2563eb' }} /> outlet
          </span>
          <span className="legend-item">
            <i style={{ background: '#93c5fd' }} /> fluid
          </span>
        </div>
      )}
    </div>
  )
}
