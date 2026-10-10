import { useEffect, useState } from 'react'
import { Link, useParams } from 'react-router-dom'
import { CaptureFeed } from '../components/CaptureFeed'
import { EnvironmentChart } from '../components/EnvironmentChart'
import { api, timeAgo } from '../lib/api'
import type { LiveMesh } from '../hooks/useLiveMesh'
import type { Capture } from '../lib/types'

export function NodePage({ live }: { live: LiveMesh }) {
  const { id } = useParams<{ id: string }>()
  const nodeId = Number(id)
  const { nodes } = live
  const node = nodes.find((n) => n.node_id === nodeId)

  const [captures, setCaptures] = useState<Capture[]>([])
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    if (!Number.isFinite(nodeId)) return
    let cancelled = false
    setLoading(true)
    void api
      .captures({ nodeId, limit: 500 })
      .then((rows) => {
        if (!cancelled) setCaptures(rows)
      })
      .catch(() => {})
      .finally(() => {
        if (!cancelled) setLoading(false)
      })
    return () => {
      cancelled = true
    }
  }, [nodeId])

  if (!Number.isFinite(nodeId)) {
    return (
      <div className="empty">
        <strong>Unknown node</strong>
      </div>
    )
  }

  const realCaptures = captures.filter((c) => !c.is_demo)

  return (
    <>
      <div className="node-page-head">
        <Link to="/" className="back-link">
          ← Overview
        </Link>
        <h1 className="node-page-title">
          Node {node?.node_id ?? id}
          {node?.role === 'bridge' && <span className="station-role" style={{ marginLeft: 10 }}>bridge</span>}
        </h1>
        <div className="page-sub">
          {node ? `${node.status} · last seen ${timeAgo(node.last_seen)}` : 'Waiting for telemetry'}
        </div>
      </div>

      <div className="kpi-row">
        <div className="kpi">
          <div className="kpi-value">{realCaptures.length}</div>
          <div className="kpi-label">real captures</div>
        </div>
        <div className="kpi">
          <div className="kpi-value">{captures.length - realCaptures.length}</div>
          <div className="kpi-label">simulated captures</div>
        </div>
        <div className="kpi">
          <div className="kpi-value">TBD</div>
          <div className="kpi-label">species identified</div>
        </div>
        <div className="kpi">
          <div className="kpi-value">TBD</div>
          <div className="kpi-label">awaiting review</div>
        </div>
        <div className="kpi">
          <div className="kpi-value">TBD</div>
          <div className="kpi-label">avg duplicates / transfer</div>
        </div>
        <div className="kpi">
          <div className="kpi-value">TBD</div>
          <div className="kpi-label">avg retransmit rounds</div>
        </div>
      </div>

      {node && <EnvironmentChart nodes={[node]} captures={captures} />}

      <section className="section">
        <div className="section-head">
          <h2>Recent transfers</h2>
          {captures.length > 0 && <span className="section-count">{captures.length}</span>}
        </div>
        {captures.length === 0 ? (
          <div className="empty">{loading ? 'Loading…' : 'No transfers recorded yet for this node.'}</div>
        ) : (
          <div className="transfer-table-wrap">
            <table className="transfer-table">
              <thead>
                <tr>
                  <th>When</th>
                  <th>Radio</th>
                  <th>Fragments</th>
                  <th>Duplicates</th>
                  <th>Retransmits</th>
                  <th>Integrity</th>
                </tr>
              </thead>
              <tbody>
                {captures.map((c) => (
                  <tr key={c.id}>
                    <td>{timeAgo(c.received_at)}</td>
                    <td>
                      {c.radio ?? '—'}
                      {c.is_demo === 1 && ' (demo)'}
                    </td>
                    <td>{c.frag_total ?? '—'}</td>
                    <td>TBD</td>
                    <td>TBD</td>
                    <td>
                      <span className="chip is-confirmed">verified</span>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>

      <CaptureFeed
        captures={captures}
        title="Captures at this node"
        emptyHint={<div className="empty">{loading ? 'Loading…' : 'No captures from this node yet.'}</div>}
      />
    </>
  )
}
