import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api } from '../lib/api'
import { useFilters, useHref, type TriggerFilter } from '../lib/filters'
import { captureName, clock, dayLabel, imageFile, routeLabel, secs, size, soundName } from '../lib/format'
import { usePolling } from '../hooks/usePolling'
import { RadioTag, SourceTag, TriggerChip } from './ui'

/** Capture records with every filter, plus trigger and search. Newest 20, then 20 more per click. */
export function CaptureTable() {
  const { filters, setFilters, key } = useFilters()
  const [limit, setLimit] = useState(20)
  const [q, setQ] = useState(filters.q ?? '')
  const navigate = useNavigate()
  const href = useHref()
  const { data, error } = usePolling((s) => api.captures(filters, limit, s), `${key}|${limit}`)

  return (
    <div className="records">
      <div className="records-tools">
        <label className="fb-field">
          <span>Trigger</span>
          <select
            value={filters.trigger ?? ''}
            onChange={(e) => setFilters({ trigger: (e.target.value || null) as TriggerFilter | null })}
          >
            <option value="">Any</option>
            <option value="dual">PIR + sound</option>
            <option value="pir">PIR only</option>
            <option value="sound">sound only</option>
            <option value="manual">manual</option>
          </select>
        </label>
        <form
          onSubmit={(e) => {
            e.preventDefault()
            setFilters({ q: q.trim() || null })
          }}
        >
          <input
            className="search"
            placeholder="Search sound class or capture id"
            value={q}
            onChange={(e) => setQ(e.target.value)}
            onBlur={() => setFilters({ q: q.trim() || null })}
          />
        </form>
        <span className="muted">{data ? `${data.captures.length} of ${data.total} shown` : ''}</span>
      </div>
      {error && <div className="error-line">Could not load captures: {error}</div>}
      <div className="tbl-wrap">
        <table className="tbl is-click">
          <thead>
            <tr>
              <th>Received</th>
              <th>Capture</th>
              <th>Node</th>
              <th>Trigger</th>
              <th>Sound class</th>
              <th>Image radio</th>
              <th>Frags</th>
              <th>Route</th>
              <th>Transfer</th>
              <th>Size</th>
              <th>Source</th>
            </tr>
          </thead>
          <tbody>
            {data?.captures.map((c) => {
              const img = imageFile(c)
              return (
                <tr key={c.id} onClick={() => navigate(href(`/capture/${c.id}`))} tabIndex={0} onKeyDown={(e) => e.key === 'Enter' && navigate(href(`/capture/${c.id}`))}>
                  <td className="mono">
                    {dayLabel(c.received_at)} {clock(c.received_at)}
                  </td>
                  <td className="mono">{captureName(c)}</td>
                  <td>Node {c.node_id}</td>
                  <td>
                    <TriggerChip capture={c} />
                  </td>
                  <td>
                    {c.sound_label ? `${soundName(c.sound_label)}${c.sound_conf != null ? ` ${c.sound_conf}%` : ''}` : <span className="muted">unavailable</span>}
                  </td>
                  <td>{img ? <RadioTag radio={img.radio} /> : <span className="muted">no image file</span>}</td>
                  <td className="num">{img?.frag_count ?? c.frag_total ?? '—'}</td>
                  <td>{routeLabel(img) ?? <span className="muted">—</span>}</td>
                  <td>{img?.radio_s != null ? secs(img.radio_s) : <span className="muted">—</span>}</td>
                  <td className="num">{size(img?.size_bytes ?? c.size_bytes)}</td>
                  <td>
                    <SourceTag demo={c.is_demo} />
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
        {data && data.captures.length === 0 && <div className="empty-line">No captures match these filters.</div>}
      </div>
      {data && data.captures.length < data.total && (
        <button className="btn btn-ghost" onClick={() => setLimit((l) => l + 20)}>
          Show 20 more
        </button>
      )}
    </div>
  )
}
