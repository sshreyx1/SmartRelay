import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react'

/** Fires whenever the WebSocket says something changed, so every poller refetches at once
 *  instead of waiting for its next 2-second tick. */
export const refreshBus = new EventTarget()
export function requestRefresh() {
  refreshBus.dispatchEvent(new Event('refresh'))
}

/** Successful REST updates, for the header's "Updated 12:27:41 · every 2 s". */
export const updatedBus = new EventTarget()

export interface Polled<T> {
  data: T | null
  error: string | null
  loading: boolean
  refresh: () => void
}

/**
 * Calls fn now, then every `intervalMs` (default 2 s), and whenever `key` changes. Paused while the
 * tab is hidden (document.hidden) and resumed with an immediate fetch when it comes back. REST is
 * the source of truth; the WebSocket only triggers an early refresh through refreshBus.
 */
export function usePolling<T>(
  fn: (signal: AbortSignal) => Promise<T>,
  key: string,
  intervalMs = 2000,
): Polled<T> {
  const [data, setData] = useState<T | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const fnRef = useRef(fn)
  useLayoutEffect(() => {
    fnRef.current = fn
  })
  const inFlight = useRef<AbortController | null>(null)
  const keyRef = useRef(key)

  const run = useCallback(() => {
    if (document.hidden) return
    inFlight.current?.abort()
    const ctl = new AbortController()
    inFlight.current = ctl
    const forKey = keyRef.current
    fnRef
      .current(ctl.signal)
      .then((d) => {
        if (ctl.signal.aborted || forKey !== keyRef.current) return
        setData(d)
        setError(null)
        updatedBus.dispatchEvent(new Event('ok'))
      })
      .catch((e: unknown) => {
        if (ctl.signal.aborted) return
        setError(e instanceof Error ? e.message : String(e))
      })
      .finally(() => {
        if (!ctl.signal.aborted) setLoading(false)
      })
  }, [])

  useEffect(() => {
    keyRef.current = key
    setLoading(true)
    run()
    const t = window.setInterval(run, intervalMs)
    const onVis = () => {
      if (!document.hidden) run()
    }
    document.addEventListener('visibilitychange', onVis)
    refreshBus.addEventListener('refresh', run)
    return () => {
      window.clearInterval(t)
      document.removeEventListener('visibilitychange', onVis)
      refreshBus.removeEventListener('refresh', run)
      inFlight.current?.abort()
    }
  }, [key, intervalMs, run])

  return { data, error, loading, refresh: run }
}
