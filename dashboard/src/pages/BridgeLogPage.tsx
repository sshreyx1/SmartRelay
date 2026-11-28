import { useState } from 'react'
import { api } from '../lib/api'
import { clock, dayLabel } from '../lib/format'
import { usePolling } from '../hooks/usePolling'
import { Panel, Segmented } from '../components/ui'
import type { LogLine } from '../lib/types'

type Kind = 'all' | 'errors' | 'nack' | 'fail'

/** Newest 20 bridge lines on every refresh; older pages load on request and are not kept growing. */
export function BridgeLogPage() {
  const [kind, setKind] = useState<Kind>('all')
  const [showEvt, setShowEvt] = useState(false)
  const [older, setOlder] = useState<LogLine[]>([])
  const [loading, setLoading] = useState(false)
  const key = `${kind}|${showEvt}`
  const { data, error } = usePolling((s) => api.log({ limit: 20, kind, showEvt }, s), key)
  const lines = [...(data ?? []), ...older.filter((o) => !(data ?? []).some((d) => d.id === o.id))]

  const loadOlder = async () => {
    const last = lines[lines.length - 1]
    if (!last) return
    setLoading(true)
    try {
      const more = await api.log({ limit: 20, kind, showEvt, beforeId: last.id })
      setOlder((o) => [...o, ...more])
    } finally {
      setLoading(false)
    }
  }

  return (
    <div className="page">
      <div className="page-head">
        <div>
          <h1>Bridge log</h1>
          <p className="page-sub">Text printed by Node 1 over USB, newest first. File data lines (@ALFP / @D) are not stored.</p>
        </div>
      </div>
      <Panel
        right={
          <label className="check">
            <input
              type="checkbox"
              checked={showEvt}
              onChange={(e) => {
                setShowEvt(e.target.checked)
                setOlder([])
              }}
            />
            show transfer events
          </label>
        }
        title={
          <Segmented<Kind>
            label="Filter"
            value={kind}
            onChange={(k) => {
              setKind(k)
              setOlder([])
            }}
            options={[
              { value: 'all', label: 'All' },
              { value: 'errors', label: 'Errors' },
              { value: 'nack', label: 'NACK' },
              { value: 'fail', label: 'Fail' },
            ]}
          />
        }
      >
        {error && <div className="error-line">Could not load the log: {error}</div>}
        <div className="tbl-wrap">
          <table className="tbl log-tbl">
            <thead>
              <tr>
                <th>Time</th>
                <th>Line</th>
              </tr>
            </thead>
            <tbody>
              {lines.map((l) => (
                <tr key={l.id} className={/fail|error|REJECT|ABORT|WARN/i.test(l.line) ? 'is-bad' : ''}>
                  <td className="mono nowrap">
                    {dayLabel(l.received_at)} {clock(l.received_at)}
                  </td>
                  <td className="mono">{l.line}</td>
                </tr>
              ))}
            </tbody>
          </table>
          {data && lines.length === 0 && <div className="empty-line">No lines match.</div>}
        </div>
        {lines.length > 0 && (
          <button className="btn btn-ghost" onClick={() => void loadOlder()} disabled={loading}>
            {loading ? 'Loading…' : 'Load older'}
          </button>
        )}
      </Panel>
    </div>
  )
}
