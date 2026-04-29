import { useEffect, useMemo, useRef } from 'react'
import uPlot from 'uplot'
import 'uplot/dist/uPlot.min.css'

export type VoltagePoint = { atMs: number; volts: number }

type Props = {
  points: VoltagePoint[]
  windowSec?: number
}

export function VoltageChart({ points, windowSec = 60 }: Props) {
  const rootRef = useRef<HTMLDivElement | null>(null)
  const plotRef = useRef<uPlot | null>(null)

  const data = useMemo(() => {
    if (points.length === 0) return [[], []] as [number[], number[]]
    const now = Date.now()
    const from = now - windowSec * 1000

    const xs: number[] = []
    const ys: number[] = []
    for (let i = points.length - 1; i >= 0; i--) {
      const p = points[i]
      if (p.atMs < from) break
      xs.push(p.atMs / 1000) // uPlot uses seconds
      ys.push(p.volts)
    }
    xs.reverse()
    ys.reverse()
    return [xs, ys] as [number[], number[]]
  }, [points, windowSec])

  useEffect(() => {
    const el = rootRef.current
    if (!el) return

    const opts: uPlot.Options = {
      width: el.clientWidth,
      height: 260,
      cursor: { drag: { x: true, y: false } },
      scales: { x: { time: true } },
      series: [
        {},
        {
          label: 'Voltage (V)',
          stroke: '#22c55e',
          width: 2,
        },
      ],
      axes: [
        { stroke: '#94a3b8', grid: { stroke: 'rgba(148,163,184,0.15)' } },
        { stroke: '#94a3b8', grid: { stroke: 'rgba(148,163,184,0.15)' } },
      ],
    }

    const plot = new uPlot(opts, data as unknown as uPlot.AlignedData, el)
    plotRef.current = plot

    const ro = new ResizeObserver(() => {
      if (!plotRef.current || !rootRef.current) return
      plotRef.current.setSize({ width: rootRef.current.clientWidth, height: 260 })
    })
    ro.observe(el)

    return () => {
      ro.disconnect()
      plot.destroy()
      plotRef.current = null
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  useEffect(() => {
    plotRef.current?.setData(data as unknown as uPlot.AlignedData)
  }, [data])

  return <div ref={rootRef} />
}

