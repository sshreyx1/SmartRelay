import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import { api } from '../lib/api'
import { captureName, clock, radioName, soundName } from '../lib/format'
import { requestRefresh, updatedBus, usePolling } from '../hooks/usePolling'
import type {
  AppNotification,
  BridgeState,
  ConnectionState,
  HealthInfo,
  MeshNode,
  NotificationLevel,
  Transfer,
  WsEvent,
} from '../lib/types'

const MAX_NOTIFICATIONS = 20
const KEEPALIVE_MS = 25_000
/** A finished upload stays on the mesh card this long so the eye can catch it. */
const DONE_LINGER_MS = 2500

/** A file the bridge is receiving right now (@EVT lines). `have` = distinct fragments so far. */
export interface LiveTransfer extends Partial<Transfer> {
  id: number
  node_id: number
  have: number
  finishedAt?: number
  /** wall-clock ms of the newest real event for this file, drives the packet dot */
  lastEventMs: number
  last_via?: number | null
  last_hops?: number | null
  elapsed_s?: number | null
}

export interface Live {
  connection: ConnectionState
  health: HealthInfo | null
  bridge: BridgeState | null
  nodes: MeshNode[]
  arriving: LiveTransfer[]
  lastUpdated: number | null
  notifications: AppNotification[]
  unread: number
  markAllRead: () => void
  reducedMotion: boolean
  setReducedMotion: (v: boolean) => void
}

const LiveCtx = createContext<Live | null>(null)

export function useLive(): Live {
  const v = useContext(LiveCtx)
  if (!v) throw new Error('useLive outside LiveProvider')
  return v
}

function prefersReduced(): boolean {
  return typeof window !== 'undefined' && window.matchMedia('(prefers-reduced-motion: reduce)').matches
}

export function LiveProvider({ children }: { children: ReactNode }) {
  const [connection, setConnection] = useState<ConnectionState>('connecting')
  const [bridge, setBridge] = useState<BridgeState | null>(null)
  const [arrivingMap, setArriving] = useState<Map<number, LiveTransfer>>(new Map())
  const [notifications, setNotifications] = useState<AppNotification[]>([])
  const [lastUpdated, setLastUpdated] = useState<number | null>(null)
  const [reducedMotion, setReducedMotion] = useState(prefersReduced)
  const notifId = useRef(0)

  useEffect(() => {
    document.documentElement.dataset.motion = reducedMotion ? 'reduced' : 'full'
  }, [reducedMotion])

  useEffect(() => {
    const onOk = () => setLastUpdated(Date.now() / 1000)
    updatedBus.addEventListener('ok', onOk)
    return () => updatedBus.removeEventListener('ok', onOk)
  }, [])

  const notify = useCallback(
    (level: NotificationLevel, title: string, body: string, tag?: AppNotification['tag']) => {
      setNotifications((prev) =>
        [{ id: notifId.current++, level, title, body, at: new Date(), read: false, tag }, ...prev].slice(
          0,
          MAX_NOTIFICATIONS,
        ),
      )
    },
    [],
  )

  const upsert = useCallback((id: number, patch: Partial<LiveTransfer>) => {
    setArriving((prev) => {
      const next = new Map(prev)
      const old = next.get(id)
      next.set(id, { have: 0, ...(old ?? {}), ...patch, id, lastEventMs: Date.now() } as LiveTransfer)
      return next
    })
  }, [])

  const finish = useCallback((id: number) => {
    setArriving((prev) => {
      const cur = prev.get(id)
      if (!cur) return prev
      const next = new Map(prev)
      next.set(id, { ...cur, finishedAt: Date.now() })
      return next
    })
    window.setTimeout(() => {
      setArriving((prev) => {
        if (!prev.has(id)) return prev
        const next = new Map(prev)
        next.delete(id)
        return next
      })
    }, DONE_LINGER_MS)
  }, [])

  // --- 2-second REST poll: bridge health, node status, files in flight -------------------------
  const health = usePolling((s) => api.health(s), 'health')
  const nodesPoll = usePolling((s) => api.nodes(s), 'nodes')
  const inFlightPoll = usePolling((s) => api.inFlight(s), 'inflight')

  useEffect(() => {
    if (health.data) setBridge(health.data.bridge)
  }, [health.data])

  // REST is the source of truth for what is in flight; keep WS progress for rows it still lists.
  useEffect(() => {
    const rows = inFlightPoll.data
    if (!rows) return
    setArriving((prev) => {
      const next = new Map<number, LiveTransfer>()
      for (const t of rows) {
        const old = prev.get(t.id)
        next.set(t.id, {
          ...t,
          last_via: old?.last_via,
          last_hops: old?.last_hops,
          lastEventMs: old?.lastEventMs ?? Date.now(),
        })
      }
      for (const [id, t] of prev) if (t.finishedAt && !next.has(id)) next.set(id, t)
      return next
    })
  }, [inFlightPoll.data])

  // Bridge connect / disconnect also from the poll, in case the WS push was missed.
  const prevBridge = useRef<boolean | null>(null)
  useEffect(() => {
    if (!bridge) return
    if (prevBridge.current !== null && prevBridge.current !== bridge.connected) {
      const at = clock(bridge.since ?? Date.now() / 1000)
      if (bridge.connected) notify('info', 'Bridge reconnected', `Node 1 USB link back at ${at}`)
      else notify('error', 'Bridge disconnected', `Node 1 USB link lost at ${at}`)
    }
    prevBridge.current = bridge.connected
  }, [bridge, notify])

  // Neutral note only: "No capture from Node 2 in 10 min". Never an "offline" alarm.
  const prevStatus = useRef<Record<number, string>>({})
  useEffect(() => {
    for (const n of nodesPoll.data ?? []) {
      if (n.role !== 'field') continue
      const was = prevStatus.current[n.node_id]
      if (was === 'recent' && n.status === 'stale') {
        notify('info', `No capture from Node ${n.node_id} in 10 min`, 'Based on captures received, not a heartbeat.')
      }
      prevStatus.current[n.node_id] = n.status
    }
  }, [nodesPoll.data, notify])

  // --- WebSocket: fast path ------------------------------------------------------------------
  const handle = useCallback(
    (msg: WsEvent) => {
      switch (msg.event) {
        case 'snapshot':
          setBridge(msg.data.bridge)
          for (const t of msg.data.in_flight) upsert(t.id, t)
          break
        case 'capture': {
          const c = msg.data
          const tag = c.is_demo ? 'SIMULATED' : 'REAL'
          notify(
            'info',
            c.is_demo ? 'New simulated capture' : 'New capture',
            `Node ${c.node_id} · ${captureName(c)} · sound class ${soundName(c.sound_label)}`,
            tag,
          )
          requestRefresh()
          break
        }
        case 'transfer_start': {
          const d = msg.data
          upsert(d.transfer_id, { node_id: d.node_id, sid: d.sid, radio: d.radio, first_at: d.first_at, is_demo: d.is_demo, have: 0 })
          if (!d.is_demo) notify('info', 'Transfer started', `Node ${d.node_id} → bridge over ${radioName(d.radio)}`, 'REAL')
          break
        }
        case 'transfer_progress': {
          const d = msg.data
          upsert(d.transfer_id, {
            node_id: d.node_id,
            sid: d.sid,
            radio: d.radio,
            have: d.have,
            frag_count: d.frag_count,
            kind: d.kind,
            capture_id: d.capture_id,
            last_hops: d.last_hops,
            last_via: d.last_via,
            via_counts: d.via_counts,
            nacks: d.nacks,
            dups: d.dups,
            elapsed_s: d.elapsed_s,
            is_demo: d.is_demo,
          })
          break
        }
        case 'transfer_done':
          upsert(msg.data.id, msg.data)
          finish(msg.data.id)
          break
        case 'transfer_failed': {
          const d = msg.data
          upsert(d.transfer_id, { node_id: d.node_id, sid: d.sid, status: 'failed', fail_reason: d.reason, is_demo: d.is_demo })
          finish(d.transfer_id)
          notify(
            'error',
            'Transfer failed',
            `Node ${d.node_id} session ${d.sid}: ${d.reason}`,
            d.is_demo ? 'SIMULATED' : 'REAL',
          )
          requestRefresh()
          break
        }
        case 'bridge_status':
          setBridge(msg.data)
          break
        case 'log':
          break
      }
    },
    [notify, upsert, finish],
  )

  useEffect(() => {
    let closed = false
    let socket: WebSocket | null = null
    let retry = 0
    let retryTimer = 0
    const connect = () => {
      if (closed) return
      const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws'
      socket = new WebSocket(`${protocol}://${window.location.host}/ws`)
      socket.onopen = () => {
        retry = 0
        setConnection('live')
      }
      socket.onmessage = (e) => {
        try {
          handle(JSON.parse(e.data) as WsEvent)
        } catch {
          // a malformed frame must not take the dashboard down
        }
      }
      socket.onclose = () => {
        setConnection('offline')
        if (closed) return
        retry = Math.min(retry + 1, 6)
        retryTimer = window.setTimeout(connect, retry * 1000)
      }
    }
    connect()
    const keepalive = window.setInterval(() => {
      if (socket?.readyState === WebSocket.OPEN) socket.send('ping')
    }, KEEPALIVE_MS)
    return () => {
      closed = true
      window.clearInterval(keepalive)
      window.clearTimeout(retryTimer)
      socket?.close()
    }
  }, [handle])

  const nodes = useMemo(() => {
    const live = new Set([...arrivingMap.values()].filter((t) => !t.finishedAt).map((t) => t.node_id))
    return (nodesPoll.data ?? []).map((n) => ({ ...n, arriving: n.arriving || live.has(n.node_id) }))
  }, [nodesPoll.data, arrivingMap])

  const arriving = useMemo(() => [...arrivingMap.values()].sort((a, b) => a.id - b.id), [arrivingMap])

  const value: Live = {
    connection,
    health: health.data,
    bridge,
    nodes,
    arriving,
    lastUpdated,
    notifications,
    unread: notifications.filter((n) => !n.read).length,
    markAllRead: () => setNotifications((p) => p.map((n) => ({ ...n, read: true }))),
    reducedMotion,
    setReducedMotion,
  }

  return <LiveCtx.Provider value={value}>{children}</LiveCtx.Provider>
}
