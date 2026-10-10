import { useEffect, useRef, useState } from 'react'
import type { AppNotification } from '../lib/types'

interface Props {
  notifications: AppNotification[]
  unreadCount: number
  onMarkAllRead: () => void
}

export function NotificationCenter({ notifications, unreadCount, onMarkAllRead }: Props) {
  const [open, setOpen] = useState(false)
  const [toast, setToast] = useState<AppNotification | null>(null)
  const rootRef = useRef<HTMLDivElement>(null)
  const seenLatest = useRef<number | null>(null)

  useEffect(() => {
    const latest = notifications[0]
    if (latest && latest.id !== seenLatest.current) {
      seenLatest.current = latest.id
      setToast(latest)
      const t = setTimeout(() => setToast(null), 6000)
      return () => clearTimeout(t)
    }
  }, [notifications])

  useEffect(() => {
    const onClick = (e: MouseEvent) => {
      if (rootRef.current && !rootRef.current.contains(e.target as Node)) setOpen(false)
    }
    document.addEventListener('mousedown', onClick)
    return () => document.removeEventListener('mousedown', onClick)
  }, [])

  return (
    <>
      <div className="notif-root" ref={rootRef}>
        <button
          className="notif-bell"
          onClick={() => {
            setOpen((o) => !o)
            if (!open) onMarkAllRead()
          }}
          aria-label={`Notifications${unreadCount ? `, ${unreadCount} unread` : ''}`}
        >
          <BellIcon />
          {unreadCount > 0 && <span className="notif-dot">{unreadCount > 9 ? '9+' : unreadCount}</span>}
        </button>

        {open && (
          <div className="notif-dropdown">
            <div className="notif-dropdown-head">Notifications</div>
            {notifications.length === 0 ? (
              <div className="notif-empty">Nothing to report. The mesh is behaving.</div>
            ) : (
              <div className="notif-list">
                {notifications.map((n) => (
                  <div className={`notif-item level-${n.level}`} key={n.id}>
                    <div className="notif-item-title">{n.title}</div>
                    <div className="notif-item-body">{n.body}</div>
                    <div className="notif-item-time">
                      {n.at.toLocaleTimeString('en-GB', { hour12: false })}
                    </div>
                  </div>
                ))}
              </div>
            )}
          </div>
        )}
      </div>

      {toast && (
        <div className={`toast level-${toast.level}`} onClick={() => setToast(null)}>
          <div className="toast-title">{toast.title}</div>
          <div className="toast-body">{toast.body}</div>
        </div>
      )}
    </>
  )
}

const BellIcon = () => (
  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
    <path d="M18 8a6 6 0 0 0-12 0c0 7-3 9-3 9h18s-3-2-3-9" />
    <path d="M13.73 21a2 2 0 0 1-3.46 0" />
  </svg>
)
