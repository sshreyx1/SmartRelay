import { useEffect, useRef, useState } from 'react'
import { clock } from '../lib/format'
import { useLive } from '../state/LiveContext'
import type { AppNotification } from '../lib/types'

export function Notifications() {
  const { notifications, unread, markAllRead } = useLive()
  const [open, setOpen] = useState(false)
  const [toast, setToast] = useState<AppNotification | null>(null)
  const root = useRef<HTMLDivElement>(null)
  const seen = useRef<number | null>(null)

  useEffect(() => {
    const latest = notifications[0]
    if (latest && latest.id !== seen.current) {
      seen.current = latest.id
      setToast(latest)
      const t = window.setTimeout(() => setToast(null), 5000)
      return () => window.clearTimeout(t)
    }
  }, [notifications])

  useEffect(() => {
    const onDown = (e: MouseEvent) => {
      if (root.current && !root.current.contains(e.target as Node)) setOpen(false)
    }
    document.addEventListener('mousedown', onDown)
    return () => document.removeEventListener('mousedown', onDown)
  }, [])

  return (
    <>
      <div className="notif" ref={root}>
        <button
          className="icon-btn"
          aria-label={`Notifications${unread ? `, ${unread} unread` : ''}`}
          onClick={() => {
            setOpen((o) => !o)
            markAllRead()
          }}
        >
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" aria-hidden>
            <path d="M18 8a6 6 0 0 0-12 0c0 7-3 9-3 9h18s-3-2-3-9" />
            <path d="M13.73 21a2 2 0 0 1-3.46 0" />
          </svg>
          {unread > 0 && <span className="badge">{unread > 9 ? '9+' : unread}</span>}
        </button>
        {open && (
          <div className="notif-menu">
            <div className="notif-head">Notifications · newest 20</div>
            {notifications.length === 0 ? (
              <div className="notif-empty">Nothing yet. New captures and bridge events appear here.</div>
            ) : (
              notifications.map((n) => <Item key={n.id} n={n} />)
            )}
          </div>
        )}
      </div>
      {toast && (
        <div className={`toast lvl-${toast.level}`} role="status" onClick={() => setToast(null)}>
          <Item n={toast} />
        </div>
      )}
    </>
  )
}

function Item({ n }: { n: AppNotification }) {
  return (
    <div className={`notif-item lvl-${n.level}`}>
      <div className="notif-title">
        {n.title}
        {n.tag && <span className={n.tag === 'REAL' ? 'tag is-real' : 'tag is-sim'}>{n.tag}</span>}
      </div>
      <div className="notif-body">{n.body}</div>
      <div className="notif-time">{clock(n.at.getTime() / 1000)}</div>
    </div>
  )
}
