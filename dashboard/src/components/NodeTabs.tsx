import { useLocation, useNavigate } from 'react-router-dom'
import { useFilters, useHref } from '../lib/filters'
import { ago, FIELD_NODES } from '../lib/format'
import { useNow } from '../hooks/useNow'
import { useLive } from '../state/LiveContext'

const RECENT_S = 600

/** All nodes · Node 1 Bridge · Node 2-5. Dot: green = last capture < 10 min, grey = older, blue = file arriving now. */
export function NodeTabs({ total }: { total: number | null }) {
  const { nodes } = useLive()
  const now = useNow()
  const { filters, setFilters } = useFilters()
  const { pathname } = useLocation()
  const navigate = useNavigate()
  const href = useHref()
  const onNodes = pathname.startsWith('/nodes')
  const bridgeOpen = pathname === '/nodes/1'

  const pickField = (id: number | null) => {
    setFilters({ node: id })
    if (onNodes && id != null) navigate({ pathname: `/nodes/${id}`, search: window.location.search })
  }

  return (
    <div className="nodetabs">
      <span className="nodetabs-label">NODES</span>
      <button
        className={'ntab' + (filters.node == null && !bridgeOpen ? ' is-on' : '')}
        onClick={() => {
          setFilters({ node: null })
          if (bridgeOpen) navigate(href('/'))
        }}
      >
        All nodes {total != null && <small>{total} captures</small>}
      </button>
      <button className={'ntab is-bridge' + (bridgeOpen ? ' is-on' : '')} onClick={() => navigate(href('/nodes/1'))}>
        Node 1 · Bridge <small>receives only</small>
      </button>
      {FIELD_NODES.map((id) => {
        const n = nodes.find((x) => x.node_id === id)
        const recent = n?.last_seen != null && now - n.last_seen < RECENT_S
        const dot = n?.arriving ? 'is-blue' : recent ? 'is-green' : 'is-grey'
        const sub = n?.arriving ? 'sending now' : n?.last_seen ? ago(n.last_seen, now) : 'no captures yet'
        return (
          <button
            key={id}
            className={'ntab' + (filters.node === id && !bridgeOpen ? ' is-on' : '')}
            onClick={() => pickField(filters.node === id ? null : id)}
            title="Last capture received from this node (not a heartbeat)"
          >
            <i className={`dot ${dot}`} /> Node {id} <small>{sub}</small>
          </button>
        )
      })}
      <span className="nodetabs-help">
        Dot = last capture &lt;10 min (green) · older (grey) · file arriving now (blue)
      </span>
    </div>
  )
}
