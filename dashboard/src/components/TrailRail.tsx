import { Link } from 'react-router-dom'
import { timeAgo } from '../lib/api'
import type { MeshNode, Stats, Theme } from '../lib/types'

interface Props {
  nodes: MeshNode[]
  stats: Stats | null
  theme: Theme
  onToggleTheme: () => void
}

/**
 * The nodes drawn as stations along a trail line -- same shape as the old
 * UI, same meaning: a chain of radios strung along a path, with the bridge
 * at the end where the line turns blue (that's where data leaves the mesh).
 *
 * Status here comes only from real captures actually received (see
 * backend/storage.py:node_last_seen()) -- the firmware has no laptop-facing
 * heartbeat, so there is no "offline" state, only "recent" / "stale" /
 * "never seen". Per-node radio health (RSSI/SNR/battery) isn't carried over
 * the current USB uplink protocol at all, so that line is marked TBD rather
 * than guessed.
 */
export function TrailRail({ nodes, stats, theme, onToggleTheme }: Props) {
  return (
    <aside className="rail">
      <div className="brand">
        <div>
          <div className="brand-mark">
            Smart<em>Relay</em>
          </div>
          <div className="brand-sub">CSC09 · ALFP trail mesh</div>
        </div>
        <button
          className="theme-toggle"
          onClick={onToggleTheme}
          aria-label={`Switch to ${theme === 'light' ? 'dark' : 'light'} theme`}
          title={`Switch to ${theme === 'light' ? 'dark' : 'light'} theme`}
        >
          {theme === 'light' ? <MoonIcon /> : <SunIcon />}
        </button>
      </div>

      <div className="rail-section">
        <div className="rail-label">Nodes</div>
        <div className="trail">
          <div className="trail-line" />
          {nodes.length === 0 && (
            <div className="station-status" style={{ paddingLeft: 4 }}>
              No nodes have reported yet.
            </div>
          )}
          {nodes.map((node) => (
            <Station key={node.node_id} node={node} />
          ))}
        </div>
      </div>

      <dl className="rail-stats">
        <div className="rail-label">Mesh health</div>
        <Stat label="images rebuilt" value={stats?.real_captures ?? 0} />
        <Stat label="simulated (demo)" value={stats?.demo_captures ?? 0} />
        <Stat label="frames received" value="TBD" />
        <Stat label="flood duplicates" value="TBD" />
        <Stat label="rejected frames" value="TBD" />
        <Stat label="integrity" value="TBD" />
      </dl>
    </aside>
  )
}

function Station({ node }: { node: MeshNode }) {
  const classes = [
    'station',
    node.status === 'recent' ? 'is-online' : '',
    node.status === 'stale' ? 'is-stale' : '',
    node.role === 'bridge' ? 'is-gateway' : '',
  ]
    .filter(Boolean)
    .join(' ')

  return (
    <Link to={`/node/${node.node_id}`} className={classes}>
      <div className="station-dot">{node.node_id}</div>
      <div className="station-body">
        <div className="station-name">
          Node {node.node_id}
          {node.role === 'bridge' && <span className="station-role">bridge</span>}
        </div>
        <div className="station-status">
          {node.status} · {timeAgo(node.last_seen)}
        </div>
        <div className="station-readout">{node.last_radio ?? '-'} radio</div>
        <div className="station-readout" style={{ opacity: 0.6 }}>
          RSSI / battery — TBD
        </div>
      </div>
    </Link>
  )
}

function Stat({ label, value }: { label: string; value: string | number }) {
  return (
    <div className="stat-line">
      <dt>{label}</dt>
      <dd>{value}</dd>
    </div>
  )
}

const MoonIcon = () => (
  <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
    <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z" />
  </svg>
)

const SunIcon = () => (
  <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
    <circle cx="12" cy="12" r="4" />
    <path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M6.34 17.66l-1.41 1.41M19.07 4.93l-1.41 1.41" />
  </svg>
)
