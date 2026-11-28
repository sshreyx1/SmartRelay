import { useState } from 'react'
import { Navigate, useNavigate, useParams } from 'react-router-dom'
import { CartesianGrid, Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from 'recharts'
import { api } from '../lib/api'
import { useFilters, useHref } from '../lib/filters'
import { ago, captureName, clock, dayLabel, imageFile, LETTER, NEIGHBOURS, radioName, relays, secs, size, soundName } from '../lib/format'
import { usePolling } from '../hooks/usePolling'
import { useNow } from '../hooks/useNow'
import { useLive } from '../state/LiveContext'
import { Kpi } from '../components/Kpi'
import { Note, Panel, RadioTag, Segmented, SourceTag, Unavailable } from '../components/ui'
import type { BridgeDetail, FieldNodeDetail, ReadingPoint, Transfer } from '../lib/types'

export function NodesIndex() {
  const { filters } = useFilters()
  const href = useHref()
  return <Navigate to={href(`/nodes/${filters.node ?? 2}`)} replace />
}

export function NodesPage() {
  const { id } = useParams<{ id: string }>()
  const nodeId = Number(id)
  if (nodeId === 1) return <BridgePage />
  if (![2, 3, 4, 5].includes(nodeId)) return <div className="page empty-line">No such node.</div>
  return <FieldNodePage nodeId={nodeId} />
}

function FieldNodePage({ nodeId }: { nodeId: number }) {
  const { filters, key } = useFilters()
  const { nodes } = useLive()
  const now = useNow()
  const { data: d, error } = usePolling((s) => api.fieldNode(nodeId, filters, s), `${nodeId}|${key}`)
  const live = nodes.find((n) => n.node_id === nodeId)
  const neighbours = NEIGHBOURS[nodeId].map((n) => (n === 1 ? 'A (bridge)' : LETTER[n])).join(', ')
  const direct = NEIGHBOURS[nodeId].includes(1)

  return (
    <div className="page">
      <div className="page-head">
        <div>
          <h1>
            Node {nodeId} · {LETTER[nodeId]} <span className="h-sub">field node · Wi-Fi neighbours {neighbours}{direct ? '' : ' · not in direct range of the bridge'}</span>
          </h1>
          <p className="page-sub">"Last capture" is the last time the bridge received a file from this node, not a live heartbeat.</p>
        </div>
      </div>
      {error && !d && <div className="error-line">Could not load node {nodeId}: {error}</div>}
      <div className="kpis is-5">
        <Kpi hero label={filters.mode === 'sim' ? 'Simulated captures' : filters.mode === 'both' ? 'Captures' : 'Real captures'} value={d?.captures_count ?? null} sub="from this node, current filters" />
        <Kpi
          label="Last capture"
          value={null}
          unavailable={<div className="kpi-value is-md">{d?.last_capture_at ? ago(d.last_capture_at, now) : 'none yet'}</div>}
          sub={`${d?.last_capture_at ? clock(d.last_capture_at) : 'no real captures'}${live?.arriving ? ' · file arriving now' : ''}`}
        />
        <Kpi
          label="Last radio used"
          value={null}
          unavailable={<div className={'kpi-value is-md ' + (d?.last_radio?.radio === 'lora' ? 'c-lora' : 'c-wifi')}>{d?.last_radio ? radioName(d.last_radio.radio) : '—'}</div>}
          sub={d?.last_radio ? `${d.last_radio.kind ?? 'file'} · ${clock(d.last_radio.received_at)}` : 'no files yet'}
        />
        <Kpi
          label="Median image transfer"
          value={null}
          unavailable={
            d?.median_image_transfer_s != null ? (
              <div className="kpi-value is-md">{secs(d.median_image_transfer_s)}</div>
            ) : (
              <Unavailable />
            )
          }
          sub={d?.median_image_transfer_s != null ? `${radioName(d.median_image_transfer_radio)} images · bridge clock` : 'needs the bridge\'s @EVT timing'}
        />
        {d?.fragments_resent ? (
          <Kpi
            label="Fragments resent"
            value={d.fragments_resent.resent}
            of={d.fragments_resent.of}
            sub="asked for again by bridge NACKs"
          />
        ) : (
          <Kpi
            label="Duplicate copies ignored"
            value={d?.files.dup ?? null}
            unavailable={d && d.files.dup_known === 0 ? <Unavailable /> : undefined}
            sub={d ? `over ${d.files.dup_known ?? 0} files · resends need @EVT lines` : ''}
          />
        )}
      </div>
      <div className="two-col">
        <Readings points={d?.readings ?? null} />
        <Delivery d={d} />
      </div>
      <History d={d} />
    </div>
  )
}

type Metric = 'temperature_c' | 'humidity_pct' | 'light_raw' | 'sound_rms'
const METRICS: { value: Metric; label: string; unit: string }[] = [
  { value: 'temperature_c', label: 'Temperature', unit: '°C' },
  { value: 'humidity_pct', label: 'Humidity', unit: '%RH' },
  { value: 'light_raw', label: 'Light', unit: 'raw' },
  { value: 'sound_rms', label: 'Sound level', unit: 'RMS' },
]

function Readings({ points }: { points: ReadingPoint[] | null }) {
  const [m, setM] = useState<Metric>('temperature_c')
  const meta = METRICS.find((x) => x.value === m)!
  const pts = (points ?? []).filter((p) => p[m] != null)
  return (
    <Panel title="Capture-time readings" right={<span className="muted-strong">one point per capture, not continuous</span>}>
      <Segmented label="Reading" value={m} onChange={setM} options={METRICS.map(({ value, label }) => ({ value, label }))} />
      {points && pts.length === 0 ? (
        <div className="unavail-panel">No {meta.label.toLowerCase()} readings in these captures (sensor sent its "missing" value).</div>
      ) : (
        <ResponsiveContainer width="100%" height={180}>
          <LineChart data={pts} margin={{ top: 12, right: 12, bottom: 0, left: -12 }}>
            <CartesianGrid vertical={false} stroke="var(--grid)" />
            <XAxis dataKey="received_at" tickFormatter={(t: number) => clock(t, false)} tick={{ fontSize: 11, fill: 'var(--text-3)' }} tickLine={false} minTickGap={30} />
            <YAxis domain={['auto', 'auto']} tick={{ fontSize: 11, fill: 'var(--text-3)' }} tickLine={false} axisLine={false} unit={m === 'temperature_c' ? '°' : ''} />
            <Tooltip
              content={({ active, payload }) => {
                if (!active || !payload?.length) return null
                const p = payload[0].payload as ReadingPoint
                return (
                  <div className="tip">
                    <strong>
                      {dayLabel(p.received_at)} {clock(p.received_at)}
                    </strong>
                    <div>
                      {meta.label}: {p[m]} {meta.unit}
                    </div>
                    {p.is_demo ? <div>SIMULATED</div> : null}
                  </div>
                )
              }}
            />
            <Line type="linear" dataKey={m} stroke="var(--forest)" strokeWidth={2} dot={{ r: 3, fill: 'var(--forest)' }} isAnimationActive={false} />
          </LineChart>
        </ResponsiveContainer>
      )}
      <div className="muted small">
        {meta.label} ({meta.unit}) · {pts.length} capture{pts.length === 1 ? '' : 's'}
      </div>
    </Panel>
  )
}

function Delivery({ d }: { d: FieldNodeDetail | null }) {
  const rc = d?.relay_counts
  const rows: [string, number, string][] = rc
    ? [
        ...Object.entries(rc.wifi)
          .sort((a, b) => b[1] - a[1])
          .map(
            ([k, v]) =>
              [k === 'direct' ? 'direct' : k === 'unknown' ? 'Wi-Fi (relay unknown)' : `via Node ${k} (${LETTER[Number(k)]})`, v, 'r-wifi'] as [
                string,
                number,
                string,
              ],
          ),
        ['LoRa (relay unknown)', rc.lora, 'r-lora'],
      ]
    : [
        ['Wi-Fi files', d?.files.wifi_files ?? 0, 'r-wifi'],
        ['LoRa files', d?.files.lora_files ?? 0, 'r-lora'],
      ]
  const max = Math.max(1, ...rows.map((r) => r[1]))
  return (
    <Panel title="How this node's files reach the bridge">
      <div className="bars">
        {rows.map(([label, v, cls]) => (
          <div className="bars-row" key={label}>
            <span>{label}</span>
            <span className="bar-cell">
              <span className={`hbar ${cls}`} style={{ width: `${(v / max) * 100}%` }} />
            </span>
            <strong>{v}</strong>
          </div>
        ))}
        {!rc && (
          <div className="bars-row">
            <span>via relay C / D / direct</span>
            <span className="bar-cell" />
            <Unavailable inline why="Needs the bridge's @EVT RXF lines (relay id per fragment)." />
          </div>
        )}
      </div>
      <Note>
        {rc
          ? 'Counts are Wi-Fi fragments by the relay that delivered them to the bridge. The LoRa header has no relay field, so LoRa shows a total only.'
          : 'Counts are completed files by radio. Fragments per relay need the relay id of each fragment, which only bridge firmware with @EVT lines reports.'}
      </Note>
    </Panel>
  )
}

function History({ d }: { d: FieldNodeDetail | null }) {
  const navigate = useNavigate()
  const href = useHref()
  return (
    <Panel title="Capture history · newest 20">
      <div className="tbl-wrap">
        <table className="tbl is-click">
          <thead>
            <tr>
              <th>Received</th>
              <th>Capture</th>
              <th>Sound class</th>
              <th>Image radio</th>
              <th>Frags</th>
              <th>Hops</th>
              <th>Relays</th>
              <th>Transfer</th>
              <th>Source</th>
            </tr>
          </thead>
          <tbody>
            {d?.captures.map((c) => {
              const img = imageFile(c)
              return (
                <tr key={c.id} onClick={() => navigate(href(`/capture/${c.id}`))}>
                  <td className="mono">{clock(c.received_at)}</td>
                  <td className="mono">{captureName(c)}</td>
                  <td>{c.sound_label ? `${soundName(c.sound_label)}${c.sound_conf != null ? ` ${c.sound_conf}%` : ''}` : <span className="muted">unavailable</span>}</td>
                  <td>{img ? <RadioTag radio={img.radio} /> : <span className="muted">—</span>}</td>
                  <td className="num">{img?.frag_count ?? c.frag_total ?? '—'}</td>
                  <td className="num">{img?.hops_max ?? '—'}</td>
                  <td>{img ? relayCell(img) : <span className="muted">—</span>}</td>
                  <td>{img?.radio_s != null ? <strong>{secs(img.radio_s)}</strong> : <span className="muted">—</span>}</td>
                  <td><SourceTag demo={c.is_demo} /></td>
                </tr>
              )
            })}
          </tbody>
        </table>
        {d && d.captures.length === 0 && <div className="empty-line">No captures from this node for these filters.</div>}
      </div>
    </Panel>
  )
}

function BridgePage() {
  const { data: b, error } = usePolling((s) => api.bridge(s), 'bridge')
  const now = useNow()
  return (
    <div className="page">
      <div className="page-head">
        <div>
          <h1>
            Node 1 · A <span className="h-sub">bridge · receives and CRC-checks files, does not make captures</span>
          </h1>
          <p className="page-sub">No sensor panels here: the bridge only receives. Its files come from the field nodes.</p>
        </div>
      </div>
      {error && !b && <div className="error-line">Could not load the bridge: {error}</div>}
      <div className="kpis is-4">
        <Kpi
          hero
          label="USB link"
          value={null}
          unavailable={<div className={'kpi-value is-md ' + (b?.bridge.connected ? 'ok' : 'bad')}>{b ? (b.bridge.connected ? 'Connected' : 'Disconnected') : '—'}</div>}
          sub={b ? `${b.bridge.port ?? 'no serial port configured'}${b.bridge.since ? ` · since ${clock(b.bridge.since)}` : ''}` : ''}
        />
        <Kpi label="Files verified" value={b?.transfers.verified ?? null} sub="bridge CRC-32 matched" />
        <Kpi
          label="Failed on the bridge"
          value={b?.transfers.failed ?? null}
          sub={
            b && b.transfers.fail_reasons && Object.keys(b.transfers.fail_reasons).length
              ? Object.entries(b.transfers.fail_reasons).map(([k, v]) => `${v} ${k}`).join(' · ')
              : `laptop CRC failures: ${b?.transfers.laptop_failed ?? 0}`
          }
        />
        <Kpi label="Receiving now" value={b?.in_flight.length ?? null} sub="verified, uploading over USB" />
      </div>
      <div className="two-col">
        <Panel title="USB link history" right={<span className="muted-strong">since the backend started</span>}>
          {b && b.bridge.history.length ? (
            <ul className="hist">
              {b.bridge.history.map((h, i) => (
                <li key={i}>
                  <i className={'dot ' + (h.connected ? 'is-green' : 'is-red')} />
                  {h.connected ? 'connected' : 'disconnected'} · {dayLabel(h.at)} {clock(h.at)} · {ago(h.at, now)}
                </li>
              ))}
            </ul>
          ) : (
            <div className="empty-line">No changes recorded.</div>
          )}
        </Panel>
        <Panel title="Receive sessions in progress">
          <InFlight b={b} />
          <Note>
            Bridge CRC mismatches (whole file asked for again): {b?.transfers.bridge_crc_retries ?? 0}. A file the bridge
            rejects is never uploaded.
          </Note>
        </Panel>
      </div>
      <Panel title="Latest bridge log · 20 lines">
        <pre className="logbox">{b?.log.map((l) => `${clock(l.received_at)}  ${l.line}`).join('\n') || 'No lines yet.'}</pre>
      </Panel>
    </div>
  )
}

function InFlight({ b }: { b: BridgeDetail | null }) {
  if (!b) return <div className="skeleton" />
  if (!b.in_flight.length) return <div className="empty-line">Nothing arriving right now.</div>
  return (
    <ul className="hist">
      {b.in_flight.map((t) => (
        <li key={t.id}>
          Node {t.node_id} · session {t.sid} · {t.have}/{t.frag_count ?? '?'} fragments{t.size_bytes != null ? ` · ${size(t.size_bytes)}` : ''} via {radioName(t.radio)}
        </li>
      ))}
    </ul>
  )
}

function relayCell(t: Transfer) {
  if (t.radio === 'lora') return <span className="muted">hops only</span>
  const r = relays(t).filter(([n]) => n !== t.node_id && n !== 1)
  if (!r.length) return <span className="muted">{Object.keys(t.via_counts ?? {}).length ? 'direct' : 'not reported'}</span>
  return <span>{r.map(([n, c]) => `${LETTER[n]} ${c}`).join(' · ')}</span>
}
