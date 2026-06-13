// Thin uPlot wrapper tuned for high-frequency streaming: the instance is
// created once, data flows through setData, width tracks the container.

import { useEffect, useRef } from 'react'
import uPlot from 'uplot'
import 'uplot/dist/uPlot.min.css'

export interface SeriesDef {
  label: string
  color: string
  width?: number
  dash?: number[]
  fill?: string
}

export interface VLine {
  x: number
  label: string
  color: string
}

interface Props {
  title: string
  xLabel: string
  series: SeriesDef[]
  data: uPlot.AlignedData
  height?: number
  logY?: boolean
  stacked?: boolean
  vlines?: VLine[]
  copyable?: boolean
}

const AXIS_STYLE: Partial<uPlot.Axis> = {
  stroke: '#7d8db1',
  grid: { stroke: 'rgba(125, 141, 177, 0.14)' },
  ticks: { stroke: 'rgba(125, 141, 177, 0.3)' },
}

export function ChartPanel({
  title,
  xLabel,
  series,
  data,
  height = 170,
  logY,
  stacked,
  vlines,
  copyable,
}: Props) {
  const hostRef = useRef<HTMLDivElement>(null)
  const plotRef = useRef<uPlot | null>(null)
  const vlinesRef = useRef<VLine[] | undefined>(vlines)
  vlinesRef.current = vlines

  useEffect(() => {
    const host = hostRef.current
    if (!host) return
    const options: uPlot.Options = {
      width: host.clientWidth || 480,
      height,
      legend: { show: true },
      cursor: { drag: { x: false, y: false } },
      hooks: {
        draw: [
          (u: uPlot) => {
            const lines = vlinesRef.current
            if (!lines) return
            const ctx = u.ctx
            for (const line of lines) {
              const px = u.valToPos(line.x, 'x', true)
              ctx.save()
              ctx.strokeStyle = line.color
              ctx.setLineDash([6, 5])
              ctx.lineWidth = 1.4
              ctx.beginPath()
              ctx.moveTo(px, u.bbox.top)
              ctx.lineTo(px, u.bbox.top + u.bbox.height)
              ctx.stroke()
              ctx.fillStyle = line.color
              ctx.font = '12px sans-serif'
              ctx.fillText(line.label, px + 5, u.bbox.top + 14)
              ctx.restore()
            }
          },
        ],
      },
      scales: {
        x: { time: false },
        y: logY ? { distr: 3, log: 10 } : { auto: true },
      },
      axes: [
        { ...AXIS_STYLE, label: xLabel, labelSize: 14, size: 36 },
        { ...AXIS_STYLE, size: 52 },
      ],
      series: [
        { label: xLabel },
        ...series.map((s) => ({
          label: s.label,
          stroke: s.color,
          width: s.width ?? 1.6,
          dash: s.dash,
          fill: s.fill,
          points: { show: false },
          ...(stacked ? { paths: uPlot.paths?.spline?.() } : {}),
        })),
      ],
    }
    const plot = new uPlot(options, [[], ...series.map(() => [])] as uPlot.AlignedData, host)
    plotRef.current = plot
    const observer = new ResizeObserver(() => {
      plot.setSize({ width: host.clientWidth, height })
    })
    observer.observe(host)
    return () => {
      observer.disconnect()
      plot.destroy()
      plotRef.current = null
    }
    // options are static by design — series defs never change at runtime
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  useEffect(() => {
    plotRef.current?.setData(data)
  }, [data])

  const copyFigure = () => {
    const canvas = hostRef.current?.querySelector('canvas')
    if (!canvas) return
    void canvas.toBlob((blob) => {
      if (blob) void navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })])
    })
  }

  return (
    <div className="sim-chart">
      <h4>
        {title}
        {copyable !== false && (
          <button type="button" className="chart-copy" title="copy figure" onClick={copyFigure}>
            ⧉
          </button>
        )}
      </h4>
      <div ref={hostRef} />
    </div>
  )
}
