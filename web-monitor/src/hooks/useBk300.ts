import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import type { Bk300Frame, Bk300Status, Bk300VoltageSample } from '@/lib/bk300'
import { Bk300Client, parseVoltageFrom4b0b } from '@/lib/bk300'
import type { VoltagePoint } from '@/components/VoltageChart'

type LogLine = {
  atMs: number
  text: string
}

function fmtTime(ms: number) {
  return new Date(ms).toLocaleTimeString(undefined, {
    hour12: false,
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  })
}

export function useBk300() {
  const client = useMemo(() => new Bk300Client(), [])
  const [status, setStatus] = useState<Bk300Status>('disconnected')
  const [isPaused, setIsPaused] = useState(false)
  const [lastVoltage, setLastVoltage] = useState<Bk300VoltageSample | null>(null)
  const [detailedSamples, setDetailedSamples] = useState<Bk300VoltageSample[]>([])
  const [bucketMap, setBucketMap] = useState<Map<number, Bucket>>(() => new Map())
  const [frames, setFrames] = useState<Bk300Frame[]>([])
  const [log, setLog] = useState<LogLine[]>([])

  const pollTimer = useRef<number | null>(null)
  const reconnectAttempt = useRef(0)
  const reconnectTimer = useRef<number | null>(null)
  const totalSamples = useRef(0)

  const retentionMs = 12 * 60 * 60 * 1000 // 12h
  const detailedMs = 10 * 60 * 1000 // last 10 minutes in full detail
  const bucketMs = 5 * 60 * 1000 // 5-minute buckets

  const appendLog = useCallback((text: string) => {
    setLog((prev) => {
      const next = [...prev, { atMs: Date.now(), text }]
      // keep last 1000 lines
      if (next.length > 1000) return next.slice(next.length - 1000)
      return next
    })
  }, [])

  const stopPolling = useCallback(() => {
    if (pollTimer.current) {
      window.clearInterval(pollTimer.current)
      pollTimer.current = null
    }
  }, [])

  const startPolling = useCallback(() => {
    stopPolling()
    // stable & proven: 0B0B -> 4B0B with voltage in payload[0..1] U16LE/100
    pollTimer.current = window.setInterval(async () => {
      if (isPaused) return
      try {
        const frame = await client.writeCommand('0b0b')
        appendLog(`→ sent 0B0B (${toHexSpaced(frame)})`)
      } catch (e) {
        appendLog(`✖ write 0B0B failed: ${(e as Error).message}`)
      }
    }, 1000)
  }, [appendLog, client, isPaused, stopPolling])

  const addVoltageSample = useCallback(
    (v: Bk300VoltageSample) => {
      totalSamples.current++
      const now = Date.now()
      const minDetailed = now - detailedMs
      const minRetention = now - retentionMs

      // 1) keep detailed samples for last N minutes
      setDetailedSamples((prev) => {
        const next = [...prev, v].filter((s) => s.atMs >= minDetailed)
        return next
      })

      // 2) aggregate into 5-minute buckets for retention period
      const bucketStart = Math.floor(v.atMs / bucketMs) * bucketMs
      setBucketMap((prev) => {
        const next = new Map(prev)
        const cur = next.get(bucketStart)
        if (cur) {
          cur.sumRaw += v.raw
          cur.count += 1
          cur.lastAtMs = v.atMs
        } else {
          next.set(bucketStart, { sumRaw: v.raw, count: 1, lastAtMs: v.atMs })
        }

        // prune old buckets
        for (const key of next.keys()) {
          if (key < minRetention - bucketMs) next.delete(key)
        }
        return next
      })
    },
    [bucketMs, detailedMs, retentionMs],
  )

  const teardown = useCallback(async () => {
    stopPolling()
    if (reconnectTimer.current) {
      window.clearTimeout(reconnectTimer.current)
      reconnectTimer.current = null
    }
    try {
      await client.disconnect()
    } catch {
      // ignore
    } finally {
      setStatus('disconnected')
    }
  }, [client, stopPolling])

  const scheduleReconnect = useCallback(() => {
    stopPolling()
    setStatus('reconnecting')
    const attempt = reconnectAttempt.current++
    const delayMs = Math.min(30_000, 1000 * 2 ** attempt)
    appendLog(`🟠 потеря связи. реконнект через ${Math.round(delayMs / 1000)}с (attempt #${attempt + 1})`)
    if (reconnectTimer.current) window.clearTimeout(reconnectTimer.current)
    reconnectTimer.current = window.setTimeout(async () => {
      try {
        if (!client.getDevice()) throw new Error('device lost')
        appendLog('🟡 reconnect: gatt.connect()')
        await client.connect()
        appendLog('🟢 подключено: notifications started on FFF1')
        reconnectAttempt.current = 0
        setStatus('connected')

        // Kick the device: 0301 -> 0204 -> 0100 (mirrors your working sequence)
        await client.writeCommand('0301')
        appendLog('→ sent 0301')
        await client.writeCommand('0204')
        appendLog('→ sent 0204')
        await client.writeCommand('0100')
        appendLog('→ sent 0100')

        startPolling()
      } catch (e) {
        appendLog(`✖ reconnect failed: ${(e as Error).message}`)
        scheduleReconnect()
      }
    }, delayMs)
  }, [appendLog, client, startPolling, stopPolling])

  const connect = useCallback(async () => {
    setStatus('connecting')
    appendLog('🟡 requestDevice…')
    try {
      await client.requestDevice()
      appendLog('🟡 gatt.connect…')

      client.setHandlers({
        onDisconnected: () => scheduleReconnect(),
        onNotifyBytes: (bytes) => {
          appendLog(`⇐ notify ${bytes.length}B: ${toHexSpaced(bytes)}`)
          if (isPaused) return
          client.pushNotifyBytes(bytes)
          const newFrames = client.popFrames()
          if (newFrames.length > 0) {
            setFrames((prev) => [...prev, ...newFrames].slice(-500))
            for (const f of newFrames) {
              appendLog(
                `⇐ frame ${f.typeHex.toUpperCase()} len=${f.length} crc=${f.crcOk ? 'ok' : 'bad'}`,
              )
              const v = parseVoltageFrom4b0b(f)
              if (v) {
                setLastVoltage(v)
                addVoltageSample(v)
              }
            }
          }
        },
      })

      await client.connect()
      appendLog('🟢 подключено: notifications started on FFF1')
      setStatus('connected')
      reconnectAttempt.current = 0

      // working kick sequence from your logs: 0301 -> 0204 -> 0100
      const f0301 = await client.writeCommand('0301')
      appendLog(`→ sent 0301 (${toHexSpaced(f0301)})`)
      const f0204 = await client.writeCommand('0204')
      appendLog(`→ sent 0204 (${toHexSpaced(f0204)})`)
      const f0100 = await client.writeCommand('0100')
      appendLog(`→ sent 0100 (${toHexSpaced(f0100)})`)

      startPolling()
    } catch (e) {
      appendLog(`✖ connect failed: ${(e as Error).message}`)
      setStatus('disconnected')
      throw e
    }
  }, [appendLog, client, isPaused, scheduleReconnect, startPolling])

  const clear = useCallback(() => {
    setLog([])
    setDetailedSamples([])
    setBucketMap(new Map())
    setFrames([])
    setLastVoltage(null)
    totalSamples.current = 0
  }, [])

  useEffect(() => {
    return () => {
      void teardown()
    }
  }, [teardown])

  const statusLabel = useMemo(() => {
    switch (status) {
      case 'disconnected':
        return '🔴 Отключено'
      case 'connecting':
        return '🟡 Подключение / Поиск…'
      case 'connected':
        return '🟢 Подключено (идёт запись)'
      case 'reconnecting':
        return '🟠 Потеря связи. Автовосстановление…'
    }
  }, [status])

  const chartPoints = useMemo<VoltagePoint[]>(() => {
    const now = Date.now()
    const cutoffDetail = now - detailedMs
    const cutoffRetention = now - retentionMs

    // buckets older than detailed window
    const bucketPoints: VoltagePoint[] = []
    for (const [start, b] of bucketMap.entries()) {
      if (start < cutoffRetention) continue
      if (start >= cutoffDetail) continue
      bucketPoints.push({ atMs: start, volts: (b.sumRaw / b.count) / 100 })
    }
    bucketPoints.sort((a, b) => a.atMs - b.atMs)

    const detailPoints: VoltagePoint[] = detailedSamples.map((s) => ({
      atMs: s.atMs,
      volts: s.volts,
    }))
    detailPoints.sort((a, b) => a.atMs - b.atMs)

    return [...bucketPoints, ...detailPoints]
  }, [bucketMap, detailedMs, detailedSamples, retentionMs])

  return {
    status,
    statusLabel,
    isPaused,
    setIsPaused,
    connect,
    disconnect: teardown,
    clear,
    log: log.map((l) => `${fmtTime(l.atMs)} ${l.text}`),
    chartPoints,
    chartSummary: {
      retentionHours: Math.round(retentionMs / (60 * 60 * 1000)),
      detailedMinutes: Math.round(detailedMs / (60 * 1000)),
      bucketMinutes: Math.round(bucketMs / (60 * 1000)),
      totalSamples: totalSamples.current,
      bucketCount: bucketMap.size,
      detailedCount: detailedSamples.length,
    },
    lastVoltage,
    frames,
  }
}

type Bucket = { sumRaw: number; count: number; lastAtMs: number }

function toHexSpaced(bytes: Uint8Array) {
  return Array.from(bytes)
    .map((b) => b.toString(16).padStart(2, '0'))
    .join(' ')
}

