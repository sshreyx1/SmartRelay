import { NavLink, useLocation } from 'react-router-dom'
import { useHref } from '../lib/filters'
import { clock } from '../lib/format'
import { useNow } from '../hooks/useNow'
import { useLive } from '../state/LiveContext'
import { Notifications } from './Notifications'

const TABS = [
  { to: '/', label: 'Overview', match: (p: string) => p === '/' },
  { to: '/gallery', label: 'Image gallery', match: (p: string) => p.startsWith('/gallery') },
  { to: '/captures', label: 'Captures', match: (p: string) => p.startsWith('/capture') },
  { to: '/nodes', label: 'Nodes', match: (p: string) => p.startsWith('/nodes') },
  { to: '/sounds', label: 'Sound classes', match: (p: string) => p.startsWith('/sounds') },
  { to: '/log', label: 'Bridge log', match: (p: string) => p.startsWith('/log') },
]

const STALE_AFTER_S = 10

export function TopBar({ onPresenter }: { onPresenter: () => void }) {
  const live = useLive()
  const now = useNow()
  const href = useHref()
  const { pathname } = useLocation()
  const { bridge, lastUpdated } = live
  const stale = lastUpdated == null || now - lastUpdated > STALE_AFTER_S

  return (
    <header className="topbar">
      <div className="brand">
        SmartRelay <span>CSC09 · wildlife trail mesh</span>
      </div>
      <nav className="tabs" aria-label="Pages">
        {TABS.map((t) => (
          <NavLink key={t.to} to={href(t.to)} className={t.match(pathname) ? 'tab is-active' : 'tab'}>
            {t.label}
          </NavLink>
        ))}
      </nav>
      <div className="topbar-right">
        {bridge == null ? (
          <span className="pill is-muted">Bridge status unknown</span>
        ) : bridge.connected ? (
          <span className="pill is-ok">
            <i className="dot" /> Bridge USB connected
          </span>
        ) : (
          <span className="pill is-bad" title={bridge.port ? `port ${bridge.port}` : 'no serial port configured'}>
            <i className="dot" /> Bridge disconnected
            {!bridge.port ? ' · no port' : bridge.since ? ` since ${clock(bridge.since)}` : ''}
          </span>
        )}
        <span className={stale ? 'pill is-warn' : 'pill is-plain'} title="REST poll every 2 s; WebSocket pushes in between">
          {lastUpdated ? `Updated ${clock(lastUpdated)}` : 'Not updated yet'} · every 2 s
        </span>
        <label className="switch">
          <span>Reduced motion</span>
          <input
            type="checkbox"
            role="switch"
            checked={live.reducedMotion}
            onChange={(e) => live.setReducedMotion(e.target.checked)}
          />
          <i aria-hidden />
        </label>
        <Notifications />
        <button className="btn btn-primary" onClick={onPresenter}>
          Presenter
        </button>
      </div>
    </header>
  )
}
