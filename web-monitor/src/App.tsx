import {
  Activity,
  ChevronDown,
  ChevronUp,
  Pause,
  Play,
  PlugZap,
  Trash2,
  Unplug,
} from 'lucide-react'
import { useMemo, useState } from 'react'

import { VoltageChart } from '@/components/VoltageChart'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { useBk300 } from '@/hooks/useBk300'

function App() {
  const {
    status,
    statusLabel,
    isPaused,
    setIsPaused,
    connect,
    disconnect,
    clear,
    log,
    samples,
    lastVoltage,
  } = useBk300()
  const [isLogOpen, setIsLogOpen] = useState(false)

  const statusVariant = useMemo(() => {
    switch (status) {
      case 'connected':
        return 'default' as const
      case 'connecting':
        return 'secondary' as const
      case 'reconnecting':
        return 'destructive' as const
      case 'disconnected':
      default:
        return 'outline' as const
    }
  }, [status])

  return (
    <div className="min-h-screen bg-background">
      <header className="border-b">
        <div className="container flex items-center justify-between py-4">
          <div className="flex items-center gap-3">
            <div className="flex h-10 w-10 items-center justify-center rounded-lg bg-primary text-primary-foreground">
              <Activity className="h-5 w-5" />
            </div>
            <div className="leading-tight">
              <div className="text-lg font-semibold">BK300 Voltage Monitor</div>
              <div className="text-xs text-muted-foreground">
                BLE · FFF0 / FFF1 notify · FFF2 write
              </div>
            </div>
          </div>

          <div className="flex items-center gap-3">
            <Badge variant={statusVariant}>{statusLabel}</Badge>
            <div className="text-right text-sm tabular-nums">
              <div className="font-medium">
                U{' '}
                <span className="text-muted-foreground">
                  {lastVoltage ? `${lastVoltage.volts.toFixed(2)} V` : '—'}
                </span>
              </div>
              <div className="text-xs text-muted-foreground">
                N {samples.length.toLocaleString()}
              </div>
            </div>
          </div>
        </div>
      </header>

      <main className="container grid gap-4 py-6">
        <Card>
          <CardHeader className="pb-3">
            <CardTitle className="text-base">Панель управления</CardTitle>
            <CardDescription>
              Подключение через Web Bluetooth. Для работы нужен Chrome/Edge и HTTPS
              (или localhost).
            </CardDescription>
          </CardHeader>
          <CardContent className="flex flex-wrap items-center gap-2">
            {status === 'disconnected' ? (
              <Button onClick={() => void connect()}>
                <PlugZap className="h-4 w-4" />
                Подключить BK300
              </Button>
            ) : (
              <Button variant="outline" onClick={() => void disconnect()}>
                <Unplug className="h-4 w-4" />
                Отключить
              </Button>
            )}

            <Button
              variant="secondary"
              onClick={() => setIsPaused((v) => !v)}
              disabled={status !== 'connected'}
            >
              {isPaused ? (
                <>
                  <Play className="h-4 w-4" />
                  Продолжить график
                </>
              ) : (
                <>
                  <Pause className="h-4 w-4" />
                  Остановить график
                </>
              )}
            </Button>

            <Button variant="ghost" onClick={clear}>
              <Trash2 className="h-4 w-4" />
              Очистить
            </Button>
          </CardContent>
        </Card>

        <Card>
          <CardHeader className="pb-3">
            <CardTitle className="text-base">График</CardTitle>
            <CardDescription>Окно последних 60 секунд</CardDescription>
          </CardHeader>
          <CardContent>
            <VoltageChart samples={samples} windowSec={60} />
          </CardContent>
        </Card>

        <Card>
          <CardHeader className="pb-3">
            <div className="flex items-start justify-between gap-3">
              <div>
                <CardTitle className="text-base">Журнал протокола</CardTitle>
                <CardDescription>Последние 1000 строк</CardDescription>
              </div>
              <Button
                variant="outline"
                size="sm"
                onClick={() => setIsLogOpen((v) => !v)}
              >
                {isLogOpen ? (
                  <>
                    <ChevronUp className="h-4 w-4" />
                    Скрыть
                  </>
                ) : (
                  <>
                    <ChevronDown className="h-4 w-4" />
                    Показать
                  </>
                )}
              </Button>
            </div>
          </CardHeader>
          {isLogOpen ? (
            <CardContent>
              <div className="h-[320px] overflow-auto rounded-md border bg-muted/20 p-3 font-mono text-xs leading-relaxed">
                {log.length === 0 ? (
                  <div className="text-muted-foreground">Пока пусто</div>
                ) : (
                  <pre className="whitespace-pre-wrap break-words">{log.join('\n')}</pre>
                )}
              </div>
            </CardContent>
          ) : null}
        </Card>
      </main>
    </div>
  )
}

export default App
