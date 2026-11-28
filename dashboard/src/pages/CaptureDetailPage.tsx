import { useState } from 'react'
import { Link, useParams } from 'react-router-dom'
import { CartesianGrid, ReferenceLine, ResponsiveContainer, Scatter, ScatterChart, Tooltip, XAxis, YAxis } from 'recharts'
import { api } from '../lib/api'
import { useHref } from '../lib/filters'
import {
  bridgeQueueSeconds,
  captureName,
  clock,
  dayLabel,
  LETTER,
  NEIGHBOURS,
  radioName,
  relays,
  routeLabel,
  secs,
  size,
  soundName,
} from '../lib/format'
import { usePolling } from '../hooks/usePolling'
import { Note, Panel, RadioTag, SourceTag, TriggerChip, Unavailable } from '../components/ui'
import type { Capture, Fragment, Transfer } from '../lib/types'

const NO_FRAGMENT_DATA =
  'Fragment-level data unavailable for this transfer (bridge firmware without transfer events).'

/** One capture: did the image arrive whole, and how was it fragmented and delivered. */
export function CaptureDetailPage() {
  const { id } = useParams<{ id: string }>()
  const rowId = Number(id)
  const href = useHref()
  const { data: c, error } = usePolling((s) => api.capture(rowId, s), `capture-${rowId}`)
  const [pick, setPick] = useState<number | null>(null)

  if (error && !c) return <div className="page error-line">Could not load capture {id}: {error}</div>
  if (!c) return <div className="page skeleton tall" />

  const files = [...c.files].sort((a, b) => (a.kind === 'image' ? 1 : 0) - (b.kind === 'image' ? 1 : 0))
  const selected = files.find((f) => f.id === pick) ?? files.find((f) => f.kind === 'image') ?? files[0]

  return (
    <div className="page detail">
      <Link className="back" to={href('/gallery')}>
        ← Image gallery
      </Link>
      <div className="detail-grid">
        <div className="detail-left">
          <CaptureCard c={c} />
          <Integrity c={c} file={files.find((f) => f.kind === 'image') ?? selected} />
          <Panel title={files.length === 2 ? 'This capture sent two files' : 'Files for this capture'} className="files-panel">
            {files.length === 0 ? (
              <div className="empty-line">No file records linked to this capture (captured before transfer logging).</div>
            ) : (
              <ul className="files">
                {files.map((f) => (
                  <li key={f.id}>
                    <button className={f.id === selected?.id ? 'is-on' : ''} onClick={() => setPick(f.id)}>
                      <span>
                        <strong>{f.kind === 'image' ? 'Image' : f.kind === 'telemetry' ? 'Telemetry' : 'File'}</strong> ·{' '}
                        {size(f.size_bytes)}
                        {f.frag_count != null && ` · ${f.frag_count} frag${f.frag_count === 1 ? '' : 's'}`}
                        {f.hops_max != null && ` · ${f.hops_max} hop${f.hops_max === 1 ? '' : 's'}`}
                      </span>
                      <RadioTag radio={f.radio} />
                      <span className="mono">{f.radio_s != null ? secs(f.radio_s) : f.upload_s != null ? `USB ${secs(f.upload_s)}` : ''}</span>
                    </button>
                  </li>
                ))}
              </ul>
            )}
            {files.length === 1 && (
              <Note>Only one of the two files is linked. Older transfers rebuilt from the bridge log can only be linked when the session id matches exactly.</Note>
            )}
          </Panel>
        </div>

        <div className="detail-right">
          {selected ? (
            <>
              <Timing f={selected} />
              <div className="detail-pair">
                <Fragments f={selected} />
                <Route f={selected} />
              </div>
              <Arrivals f={selected} />
            </>
          ) : (
            <Panel title="Transfer">
              <div className="unavail-panel">{NO_FRAGMENT_DATA}</div>
            </Panel>
          )}
        </div>
      </div>
    </div>
  )
}

function CaptureCard({ c }: { c: Capture }) {
  const tel = c.files.find((f) => f.kind === 'telemetry')
  const img = c.files.find((f) => f.kind === 'image')
  const readings = [
    c.temperature_c != null ? `${c.temperature_c.toFixed(1)} °C` : null,
    c.humidity_pct != null ? `${Math.round(c.humidity_pct)} %RH` : null,
    c.light_raw != null ? `light ${c.light_raw}` : null,
    c.weather_name ? c.weather_name.replace('_', ' ') : null,
  ].filter(Boolean)
  return (
    <section className="panel capture-card">
      {c.image_url ? (
        <a href={c.image_url} target="_blank" rel="noreferrer">
          <img src={c.image_url} alt={`Node ${c.node_id} capture`} />
        </a>
      ) : (
        <div className="no-img">No image file (telemetry only)</div>
      )}
      <div className="capture-meta">
        <h1>
          Node {c.node_id} · capture {captureName(c)}
        </h1>
        <div className="chips">
          <SourceTag demo={c.is_demo} />
          <TriggerChip capture={c} />
          {img && <span className={`tag ${img.radio === 'lora' ? 'is-lora' : 'is-wifi'}`}>{radioName(img.radio)} image</span>}
          {tel && <span className={`tag ${tel.radio === 'lora' ? 'is-lora' : 'is-wifi'}`}>{radioName(tel.radio)} telemetry</span>}
        </div>
        <p className="muted">
          Sound class <strong className="ink">{soundName(c.sound_label)}</strong>
          {c.sound_conf != null && ` ${c.sound_conf}%`} (on-node, not a confirmed species)
          {readings.length > 0 && <> · {readings.join(' · ')}</>}
        </p>
        <p className="muted small">
          Received {dayLabel(c.received_at)} {clock(c.received_at)} · capture-time readings, one per capture
        </p>
      </div>
    </section>
  )
}

/** "39 × 236 B + 207 B" from the real fragment size the bridge reported. */
function sizeBreakdown(f: Transfer | undefined): string | null {
  if (!f || f.size_bytes == null || f.frag_size == null || f.frag_count == null || f.frag_count < 1) return null
  const last = f.size_bytes - (f.frag_count - 1) * f.frag_size
  if (last <= 0 || last > f.frag_size) return null
  return f.frag_count === 1 ? `1 × ${last} B` : `${f.frag_count - 1} × ${f.frag_size} B + ${last} B`
}

function Integrity({ c, file }: { c: Capture; file: Transfer | undefined }) {
  const crc = file?.crc_sender ?? c.crc32
  const laptop = file?.crc_laptop_ok
  const bridge = file ? file.crc_bridge_ok === 1 || file.status === 'verified' : !!c.crc32
  const breakdown = sizeBreakdown(file)
  return (
    <Panel title="File integrity">
      <dl className="kv">
        <dt>Sender CRC-32</dt>
        <dd className="mono">{crc ?? <Unavailable inline />}</dd>
        <dt>Bridge CRC-32</dt>
        <dd>{bridge ? <span className="mono">{crc ?? ''} <span className="ok">✓</span></span> : <Unavailable inline />}</dd>
        <dt>Laptop re-check</dt>
        <dd>
          {laptop === 1 ? (
            <span className="mono">
              {crc ?? ''} <span className="ok">✓</span>
            </span>
          ) : laptop === 0 ? (
            <span className="bad">mismatch ✗</span>
          ) : (
            <span className="muted">not rechecked</span>
          )}
        </dd>
        <dt>Size</dt>
        <dd>
          {(file?.size_bytes ?? c.size_bytes)?.toLocaleString('en-GB') ?? '—'} B
          {breakdown
            ? ` = ${breakdown}`
            : (file?.frag_count ?? c.frag_total) != null &&
              ` in ${file?.frag_count ?? c.frag_total} fragment${(file?.frag_count ?? c.frag_total) === 1 ? '' : 's'}`}
        </dd>
      </dl>
      <Note>
        The checksum is computed over the whole rebuilt file. One wrong or missing byte would change it, so a match
        means the image arrived complete.
      </Note>
    </Panel>
  )
}

function Timing({ f }: { f: Transfer }) {
  const queued = f.queued_ms != null ? f.queued_ms / 1000 : null
  const radio = f.radio_s
  const waitBridge = bridgeQueueSeconds(f)
  const up = f.upload_s
  const parts = [queued, radio, waitBridge, up].filter((x): x is number => x != null)
  const known = parts.reduce((a, b) => a + b, 0)
  const w = (s: number | null) => (s == null || known === 0 ? 0 : Math.max(5, (s / known) * 70))
  const total = queued != null && radio != null ? queued + radio + (waitBridge ?? 0) + (up ?? 0) : null
  return (
    <Panel title="How long it took" right={<span className="muted-strong">{f.kind ?? 'file'}</span>}>
      <div className="timing">
        {queued != null ? (
          <div className="seg-queue" style={{ width: `${w(queued)}%` }} title="Time the file waited in the node's send queue (node clock)">
            <span>queued {secs(queued)}</span>
            <small>node clock</small>
          </div>
        ) : (
          <div className="seg-unknown" style={{ width: '14%' }}>
            <span>queue</span>
            <small>not reported</small>
          </div>
        )}
        <div className="seg-gap" title="Node and bridge have separate clocks, so this gap can't be measured exactly">
          <span>*</span>
        </div>
        {radio != null ? (
          <div className="seg-radio" style={{ width: `${w(radio)}%` }} title="First fragment to verified, bridge clock">
            <span>radio transfer {secs(radio)}</span>
            <small>bridge clock</small>
          </div>
        ) : (
          <div className="seg-unknown" style={{ width: '30%' }}>
            <span>radio transfer</span>
            <small>not reported by bridge</small>
          </div>
        )}
        <div className="seg-crc" title="Bridge CRC-32 matched">
          CRC ✓
        </div>
        {waitBridge != null && waitBridge > 0.05 && (
          <div className="seg-wait" style={{ width: `${w(waitBridge)}%` }}>
            <span>bridge queue {secs(waitBridge)}</span>
          </div>
        )}
        {up != null ? (
          <div className="seg-usb" style={{ width: `${w(up)}%` }}>
            <span>USB upload {secs(up)}</span>
          </div>
        ) : (
          <div className="seg-unknown" style={{ flex: 1 }}>
            <span>USB upload</span>
            <small>not timed</small>
          </div>
        )}
      </div>
      <div className="timing-axis">
        <span>{f.first_at ? `first fragment at bridge ${clock(f.first_at)}` : ''}</span>
        <span>{f.verified_at ? `verified ${clock(f.verified_at)}` : ''}</span>
        <span>{f.upload_end_at ? `on dashboard ${clock(f.upload_end_at)}` : ''}</span>
      </div>
      <p className="muted small">
        Total from trigger ≈{' '}
        {total != null ? <strong className="ink">{secs(total)}</strong> : <Unavailable inline why="Needs both queued_ms (capture meta v2) and the bridge's radio timing." />}
        . *Node and bridge have separate clocks, so the moment between "starts sending" and "first fragment" can't be
        measured exactly. It is one trip through the mesh, so the gap is small.
      </p>
    </Panel>
  )
}

function Fragments({ f }: { f: Transfer }) {
  const frags = f.fragments ?? []
  const n = f.frag_count
  const has = frags.length > 0
  const bySeq = new Map(frags.map((x) => [x.seq, x]))
  const firstTry = frags.filter((x) => (x.round ?? 0) === 0).length
  const resent = frags.length - firstTry
  const dupCopies = frags.reduce((a, x) => a + (x.dup_count ?? 0), 0)
  const fsz = f.frag_size
  return (
    <Panel
      title={`Fragmentation${n != null ? ` · ${n} piece${n === 1 ? '' : 's'}` : ''}`}
      right={
        has ? (
          <span className="legend">
            <i className="sw is-first" /> first try <i className="sw is-resent" /> resent after NACK <i className="sw is-dup" /> duplicate copy
          </span>
        ) : (
          <span className="legend">
            <i className="sw is-neutral" /> arrived (order not reported)
          </span>
        )
      }
    >
      {n == null ? (
        <div className="unavail-panel">Fragment count is not in the bridge log line for this older transfer.</div>
      ) : (
        <div className="frag-grid" aria-label={`${n} fragments`}>
          {Array.from({ length: n }, (_, i) => {
            const fr = bySeq.get(i)
            const cls = !has ? 'frag' : !fr ? 'frag is-missing' : (fr.round ?? 0) >= 1 ? 'frag is-resent' : 'frag is-first'
            const via = fr?.via ? (fr.via === f.node_id ? 'direct' : LETTER[fr.via] ?? String(fr.via)) : '–'
            const range = fsz != null ? `bytes ${i * fsz}–${Math.min((i + 1) * fsz, f.size_bytes ?? (i + 1) * fsz) - 1}` : ''
            const title = fr
              ? `fragment ${i} · ${range} · ${fr.t_rel_s != null ? `at ${fr.t_rel_s.toFixed(2)} s` : 'time not reported'} · round ${(fr.round ?? 0) + 1} · ${fr.hops ?? '?'} hops · via ${via}`
              : has
                ? `fragment ${i} · not seen in the bridge's events`
                : `fragment ${i} · arrival time, round and relay not reported`
            return (
              <span key={i} className={cls} title={title}>
                {i}
                {has && <small>{f.radio === 'lora' ? '–' : via === 'direct' ? 'A' : via}</small>}
                {fr && fr.dup_count > 0 && <b className="dup">{fr.dup_count + 1}</b>}
              </span>
            )
          })}
        </div>
      )}
      <p className="small">
        {has
          ? `${firstTry} arrived on the first try${
              resent ? `; the bridge asked for ${resent} missing one${resent === 1 ? '' : 's'} (${f.nacks ?? '?'} NACK${f.nacks === 1 ? '' : 's'}) and ${resent === 1 ? 'it' : 'they'} arrived in round ${f.rounds ?? 2}` : ''
            }. ${dupCopies} duplicate cop${dupCopies === 1 ? 'y was' : 'ies were'} ignored.`
          : `${n != null ? `All ${n} fragments arrived and the rebuilt file passed the bridge's CRC check. ` : ''}${
              f.dups != null ? `${f.dups} duplicate cop${f.dups === 1 ? 'y was' : 'ies were'} ignored. ` : ''
            }${NO_FRAGMENT_DATA}`}
      </p>
      <Note>
        Number = fragment order · letter = relay that delivered it to the bridge. Fragmentation: the node cuts the file
        into numbered pieces that each fit one radio packet; the bridge puts them back in order and asks again for any
        that are missing.
      </Note>
    </Panel>
  )
}

function Route({ f }: { f: Transfer }) {
  const src = f.node_id
  const counts = new Map(relays(f))
  const candidates = (NEIGHBOURS[src] ?? []).filter((n) => n !== 1)
  const direct = (NEIGHBOURS[src] ?? []).includes(1)
  const directCount = (counts.get(src) ?? 0) + (counts.get(1) ?? 0)
  const P: Record<string, { x: number; y: number }> = { bridge: { x: 70, y: 110 }, src: { x: 330, y: 110 } }
  candidates.forEach(
    (r, i) => (P[r] = { x: 200, y: candidates.length === 1 ? 50 : 40 + (i * 140) / Math.max(1, candidates.length - 1) }),
  )
  const width = (c: number) => (c ? Math.min(12, 3 + c / 4) : 2)
  const lora = f.radio === 'lora'
  const known = counts.size > 0 && !lora
  return (
    <Panel title="Route through the mesh">
      <svg viewBox="0 0 400 200" className="route" role="img" aria-label={`Routes from Node ${src} to the bridge`}>
        {direct && (
          <line
            x1={P.bridge.x}
            y1={P.bridge.y}
            x2={P.src.x}
            y2={P.src.y}
            className={known && directCount ? 'r-edge is-live' : 'r-edge'}
            style={{ strokeWidth: width(known ? directCount : 0) }}
          />
        )}
        {candidates.map((r) => {
          const c = known ? counts.get(r) ?? 0 : 0
          return (
            <g key={r}>
              <line x1={P.bridge.x} y1={P.bridge.y} x2={P[r].x} y2={P[r].y} className={c ? 'r-edge is-live' : 'r-edge'} style={{ strokeWidth: width(c) }} />
              <line x1={P[r].x} y1={P[r].y} x2={P.src.x} y2={P.src.y} className={c ? 'r-edge is-live' : 'r-edge'} style={{ strokeWidth: width(c) }} />
              <circle cx={P[r].x} cy={P[r].y} r="20" className="r-node" />
              <text x={P[r].x} y={P[r].y + 4} textAnchor="middle" className="r-t">
                {LETTER[r]}·{r}
              </text>
              {c > 0 && (
                <text x={P[r].x + 26} y={P[r].y + (P[r].y < 110 ? -8 : 20)} className="r-count">
                  {c} via {LETTER[r]}
                </text>
              )}
            </g>
          )
        })}
        <rect x={P.bridge.x - 50} y={P.bridge.y - 20} width="100" height="40" rx="6" className="r-bridge" />
        <text x={P.bridge.x} y={P.bridge.y + 4} textAnchor="middle" className="r-bt">
          Node 1 bridge
        </text>
        <circle cx={P.src.x} cy={P.src.y} r="24" className="r-src" />
        <text x={P.src.x} y={P.src.y + 4} textAnchor="middle" className="r-st">
          {LETTER[src]}·{src}
        </text>
      </svg>
      <p className="small">
        {lora
          ? `Sent over LoRa: ${f.hops_max != null ? `${f.hops_max} hop${f.hops_max === 1 ? '' : 's'}, ` : ''}relay unknown (the LoRa header has no relay field).`
          : known
            ? `${routeLabel(f) ?? ''}. ${[...counts.entries()]
                .filter(([n]) => n !== src && n !== 1)
                .map(([n, c]) => `${c} fragments via ${LETTER[n]}`)
                .join(', ')}. Relays flood every packet; the bridge keeps whichever copy arrives first.`
            : `Grey lines are the possible Wi-Fi routes from Node ${src}. The relay for each fragment was not reported for this transfer, so no route is highlighted.`}
      </p>
    </Panel>
  )
}

function Arrivals({ f }: { f: Transfer }) {
  const pts = (f.fragments ?? []).filter((x): x is Fragment & { t_rel_s: number } => x.t_rel_s != null)
  if (!pts.length)
    return (
      <Panel title="When each fragment arrived at the bridge">
        <div className="unavail-panel">{NO_FRAGMENT_DATA}</div>
      </Panel>
    )
  const first = pts.filter((p) => (p.round ?? 0) === 0)
  const resent = pts.filter((p) => (p.round ?? 0) >= 1)
  const round2 = resent.length ? Math.min(...resent.map((p) => p.t_rel_s)) : null
  return (
    <Panel title="When each fragment arrived at the bridge" right={<span className="muted-strong">x = seconds since first fragment · y = fragment number</span>}>
      <ResponsiveContainer width="100%" height={200}>
        <ScatterChart margin={{ top: 8, right: 16, bottom: 4, left: -16 }}>
          <CartesianGrid stroke="var(--grid)" />
          <XAxis type="number" dataKey="t_rel_s" unit=" s" tick={{ fontSize: 11, fill: 'var(--text-3)' }} />
          <YAxis type="number" dataKey="seq" allowDecimals={false} tick={{ fontSize: 11, fill: 'var(--text-3)' }} />
          <Tooltip
            content={({ active, payload }) => {
              if (!active || !payload?.length) return null
              const p = payload[0].payload as Fragment
              return (
                <div className="tip">
                  <strong>fragment {p.seq}</strong>
                  <div>
                    at {p.t_rel_s?.toFixed(2)} s · round {(p.round ?? 0) + 1}
                  </div>
                  <div>
                    {p.hops ?? '?'} hops · via {p.via ? (p.via === f.node_id ? 'direct' : LETTER[p.via]) : 'unknown'}
                  </div>
                </div>
              )
            }}
          />
          {round2 != null && (
            <ReferenceLine
              x={round2}
              stroke="var(--amber)"
              strokeDasharray="4 3"
              label={{
                value: `after ${f.nacks ?? '?'} NACK: ${resent.map((p) => p.seq).join(', ')} resent`,
                position: 'insideTopRight',
                fontSize: 11,
                fill: 'var(--amber)',
              }}
            />
          )}
          <Scatter name="first try" data={first} fill="var(--wifi)" isAnimationActive={false} />
          {resent.length > 0 && <Scatter name="resent" data={resent} fill="var(--amber)" isAnimationActive={false} />}
        </ScatterChart>
      </ResponsiveContainer>
      <div className="legend">
        <i className="sw is-first" /> first try {resent.length > 0 && <><i className="sw is-resent" /> resent after NACK</>}
      </div>
    </Panel>
  )
}
