import { useEffect, useState } from 'react'
import { BrowserRouter, Link, NavLink, Route, Routes } from 'react-router-dom'
import { ConnectionBadge } from './components/ConnectionBadge'
import { NotificationCenter } from './components/NotificationCenter'
import { TrailRail } from './components/TrailRail'
import { useLiveMesh } from './hooks/useLiveMesh'
import { useTheme } from './hooks/useTheme'
import { api } from './lib/api'
import { CapturesPage } from './pages/CapturesPage'
import { HomePage } from './pages/HomePage'
import { NodePage } from './pages/NodePage'
import { ReviewPage } from './pages/ReviewPage'
import { SpeciesPage } from './pages/SpeciesPage'
import type { HealthInfo } from './lib/types'

export default function App() {
  return (
    <BrowserRouter>
      <Shell />
    </BrowserRouter>
  )
}

function Shell() {
  const [theme, toggleTheme] = useTheme()
  const live = useLiveMesh()
  const { connection, nodes, stats, notifications, unreadCount, markAllRead } = live
  const [health, setHealth] = useState<HealthInfo | null>(null)

  // Bridge connection state doesn't change on every capture, so it's
  // polled on a slow interval rather than pushed over the socket.
  useEffect(() => {
    let cancelled = false
    const poll = () => void api.health().then((h) => !cancelled && setHealth(h)).catch(() => {})
    poll()
    const t = setInterval(poll, 5000)
    return () => {
      cancelled = true
      clearInterval(t)
    }
  }, [])

  const online = nodes.filter((n) => n.status === 'recent').length
  const total = (stats?.real_captures ?? 0) + (stats?.demo_captures ?? 0)

  const subtitle = nodes.length
    ? `${online} of ${nodes.length} nodes reporting · ${total} capture${total === 1 ? '' : 's'} held`
    : 'Waiting for the first node to report'

  return (
    <div className="app">
      <TrailRail nodes={nodes} stats={stats} theme={theme} onToggleTheme={toggleTheme} />

      <main className="main">
        <header className="page-head">
          <div>
            <h1>Mesh activity</h1>
            <div className="page-sub">
              {subtitle}
              {health && !health.bridge_connected && (
                <span style={{ color: 'var(--detect)' }}>
                  {' '}
                  · {health.bridge_port ? `bridge port set (${health.bridge_port}) but not connected` : 'no bridge serial port detected'}
                </span>
              )}
            </div>
          </div>
          <div className="page-head-actions">
            <a
              className="export-link"
              href="#"
              onClick={(e) => e.preventDefault()}
              title="TBD — no export endpoint yet"
              style={{ opacity: 0.55, cursor: 'default' }}
            >
              Export captures (TBD)
            </a>
            <NotificationCenter notifications={notifications} unreadCount={unreadCount} onMarkAllRead={markAllRead} />
            <ConnectionBadge state={connection} />
          </div>
        </header>

        <nav className="top-nav">
          <NavLink to="/" end className={({ isActive }) => 'top-nav-tab' + (isActive ? ' is-active' : '')}>
            Overview
          </NavLink>
          <NavLink to="/sightings" className={({ isActive }) => 'top-nav-tab' + (isActive ? ' is-active' : '')}>
            Captures
          </NavLink>
          <NavLink to="/species" className={({ isActive }) => 'top-nav-tab' + (isActive ? ' is-active' : '')}>
            Species
          </NavLink>
          <NavLink to="/review" className={({ isActive }) => 'top-nav-tab' + (isActive ? ' is-active' : '')}>
            Review
          </NavLink>
        </nav>

        <Routes>
          <Route path="/" element={<HomePage live={live} />} />
          <Route path="/sightings" element={<CapturesPage live={live} />} />
          <Route path="/species" element={<SpeciesPage />} />
          <Route path="/review" element={<ReviewPage />} />
          <Route path="/node/:id" element={<NodePage live={live} />} />
          <Route
            path="*"
            element={
              <div className="empty">
                <strong>Page not found</strong>
                <Link to="/" className="btn btn-ghost" style={{ marginTop: 12, display: 'inline-block' }}>
                  Back to overview
                </Link>
              </div>
            }
          />
        </Routes>
      </main>
    </div>
  )
}
