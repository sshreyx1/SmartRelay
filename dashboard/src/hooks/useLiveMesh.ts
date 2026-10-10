import { useCallback, useEffect, useRef, useState } from 'react'
import { api } from '../lib/api'
import type {
  AppNotification,
  Capture,
  ConnectionState,
  HourBucket,
  LogLine,
  MeshNode,
  NotificationLevel,
  Stats,
  WsEvent,
} from '../lib/types'

const MAX_CAPTURES = 80
const MAX_LOG = 150
const MAX_NOTIFICATIONS = 40
const KEEPALIVE_MS = 25_000

export interface LiveMesh {
  connection: ConnectionState
  nodes: MeshNode[]
  captures: Capture[]
  log: LogLine[]
  stats: Stats | null
  activity: HourBucket[]
  notifications: AppNotification[]
  unreadCount: number
  markAllRead: () => void
}

/** Hour-of-day histogram of real (non-demo) captures -- genuinely derived
 *  from what's on hand, the same way the old UI's "activity by hour" was,
 *  just computed client-side instead of by a dedicated endpoint. */
function activityFromCaptures(captures: Capture[]): HourBucket[] {
  const buckets = Array.from({ length: 24 }, (_, hour) => ({ hour, count: 0 }))
  for (const c of captures) {
    if (c.is_demo) continue
    const hour = new Date(c.received_at * 1000).getHours()
    buckets[hour].count += 1
  }
  return buckets
}

export function useLiveMesh(): LiveMesh {
  const [connection, setConnection] = useState<ConnectionState>('connecting')
  const [nodes, setNodes] = useState<MeshNode[]>([])
  const [captures, setCaptures] = useState<Capture[]>([])
  const [log, setLog] = useState<LogLine[]>([])
  const [stats, setStats] = useState<Stats | null>(null)
  const [notifications, setNotifications] = useState<AppNotification[]>([])

  const socketRef = useRef<WebSocket | null>(null)
  const retryRef = useRef(0)
  const notifIdRef = useRef(0)
  const closedByUs = useRef(false)

  const notify = useCallback((level: NotificationLevel, title: string, body: string) => {
    setNotifications((prev) =>
      [{ id: notifIdRef.current++, level, title, body, at: new Date(), read: false }, ...prev].slice(
        0,
        MAX_NOTIFICATIONS,
      ),
    )
  }, [])

  const handle = useCallback(
    (msg: WsEvent) => {
      switch (msg.event) {
        case 'snapshot':
          setNodes(msg.data.nodes)
          setCaptures(msg.data.captures)
          setLog(msg.data.log)
          setStats(msg.data.stats)
          break
        case 'capture':
          setCaptures((prev) => [msg.data, ...prev].slice(0, MAX_CAPTURES))
          setStats((prev) =>
            prev
              ? {
                  ...prev,
                  real_captures: prev.real_captures + (msg.data.is_demo ? 0 : 1),
                  demo_captures: prev.demo_captures + (msg.data.is_demo ? 1 : 0),
                }
              : prev,
          )
          void api.nodes().then(setNodes).catch(() => {})
          notify(
            'info',
            msg.data.is_demo ? 'Simulated capture' : 'New capture',
            `Node ${msg.data.node_id}` +
              (msg.data.weather_name ? ` · ${msg.data.weather_name}` : '') +
              (msg.data.sound_name ? ` · ${msg.data.sound_name}` : ''),
          )
          break
        case 'log':
          setLog((prev) => [...prev, { id: Date.now() + Math.random(), line: msg.data.line, received_at: msg.ts }].slice(-MAX_LOG))
          break
      }
    },
    [notify],
  )

  useEffect(() => {
    closedByUs.current = false

    const connect = () => {
      if (closedByUs.current) return
      const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws'
      const socket = new WebSocket(`${protocol}://${window.location.host}/ws`)
      socketRef.current = socket

      socket.onopen = () => {
        retryRef.current = 0
        setConnection('live')
      }
      socket.onmessage = (event) => {
        try {
          handle(JSON.parse(event.data) as WsEvent)
        } catch {
          // a malformed frame shouldn't take the dashboard down
        }
      }
      socket.onerror = () => setConnection('offline')
      socket.onclose = () => {
        setConnection('offline')
        if (closedByUs.current) return
        retryRef.current = Math.min(retryRef.current + 1, 6)
        setTimeout(connect, retryRef.current * 1000)
      }
    }

    connect()
    const keepalive = window.setInterval(() => {
      if (socketRef.current?.readyState === WebSocket.OPEN) socketRef.current.send('ping')
    }, KEEPALIVE_MS)

    return () => {
      closedByUs.current = true
      window.clearInterval(keepalive)
      socketRef.current?.close()
    }
  }, [handle])

  const unreadCount = notifications.filter((n) => !n.read).length
  const markAllRead = useCallback(() => {
    setNotifications((prev) => prev.map((n) => ({ ...n, read: true })))
  }, [])

  return {
    connection,
    nodes,
    captures,
    log,
    stats,
    activity: activityFromCaptures(captures),
    notifications,
    unreadCount,
    markAllRead,
  }
}
