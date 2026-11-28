import { ago, captureName, LETTER, NEIGHBOURS, radioName, secs, size } from '../lib/format'
import { useNow } from '../hooks/useNow'
import { useLive, type LiveTransfer } from '../state/LiveContext'
import type { Mode } from '../lib/types'

/**
 * "How a capture reaches the dashboard": field nodes laid out as the Wi-Fi topology in
 * third_party/wifi_mesh/wifi_mesh_config.h, Node 1 (bridge) and the laptop over USB.
 *
 * Only real events move anything (backend/transfers.py, from the bridge's @EVT lines):
 * - the sender is ringed blue while its file is arriving;
 * - links are highlighted only for relays the bridge actually reported in via_counts, with
 *   "N frags via C" labels;
 * - one dot moves per transfer_progress event (the backend throttles these to <= 4/s).
 * LoRa never reports a relay, so only hops are shown for it.
 */

const POS: Record<number, { x: number; y: number }> = {
  1: { x: 300, y: 238 },
  2: { x: 440, y: 92 },
  3: { x: 630, y: 92 },
  4: { x: 630, y: 238 },
  5: { x: 815, y: 165 },
}
const LAPTOP = { x: 92, y: 238 }
const R = 30

function edges(): [number, number][] {
  const out: [number, number][] = []
  for (const [a, ns] of Object.entries(NEIGHBOURS)) for (const b of ns) if (Number(a) < b) out.push([Number(a), b])
  return out
}
const EDGES = edges()
const key = (a: number, b: number) => (a < b ? `${a}-${b}` : `${b}-${a}`)

/** Blue links for one transfer: sender -> relay -> bridge for every relay with fragments. */
function liveLinks(t: LiveTransfer): Map<string, number> {
  const out = new Map<string, number>()
  if (t.radio === 'lora') return out
  for (const [v, c] of Object.entries(t.via_counts ?? {})) {
    const via = Number(v)
    if (!via || !c) continue
    if (via === t.node_id || via === 1) {
      out.set(key(t.node_id, 1), (out.get(key(t.node_id, 1)) ?? 0) + c)
    } else {
      out.set(key(t.node_id, via), (out.get(key(t.node_id, via)) ?? 0) + c)
      out.set(key(via, 1), (out.get(key(via, 1)) ?? 0) + c)
    }
  }
  return out
}

function dotPath(t: LiveTransfer): string | null {
  const s = POS[t.node_id]
  if (!s) return null
  const via = t.last_via
  if (t.radio === 'lora' || !via) return null
  if (via === t.node_id || via === 1) return `M${s.x},${s.y} L${POS[1].x},${POS[1].y}`
  const v = POS[via]
  return v ? `M${s.x},${s.y} L${v.x},${v.y} L${POS[1].x},${POS[1].y}` : null
}

export function MeshDiagram({
  byNode,
  mode,
  compact,
}: {
  byNode: Record<string, number> | null
  mode: Mode
  compact?: boolean
}) {
  const { nodes, arriving, reducedMotion } = useLive()
  const now = useNow()
  const active = arriving.filter((t) => (mode === 'sim' ? !!t.is_demo : mode === 'real' ? !t.is_demo : true))
  const sending = new Map<number, LiveTransfer>()
  for (const t of active) if (!t.finishedAt) sending.set(t.node_id, t)
  const showCard = active[active.length - 1]
  const links = showCard && !showCard.finishedAt ? liveLinks(showCard) : new Map<string, number>()
  const path = showCard && !showCard.finishedAt ? dotPath(showCard) : null
  const sim = !!showCard?.is_demo

  return (
    <div className={'mesh' + (compact ? ' is-compact' : '')}>
      <svg viewBox="0 0 900 290" role="img" aria-label="Mesh diagram: four field nodes, the bridge (Node 1) and the laptop">
        <defs>
          <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="9" markerHeight="9" orient="auto">
            <path d="M0,0 L10,5 L0,10 z" fill="var(--forest)" />
          </marker>
        </defs>

        {EDGES.map(([a, b]) => {
          const n = links.get(key(a, b))
          return (
            <line
              key={`${a}-${b}`}
              x1={POS[a].x}
              y1={POS[a].y}
              x2={POS[b].x}
              y2={POS[b].y}
              className={'m-edge' + (n ? (sim ? ' is-sim-live' : ' is-live') : '')}
              style={n ? { strokeWidth: Math.min(9, 3 + n / 6) } : undefined}
            />
          )
        })}
        {showCard &&
          !showCard.finishedAt &&
          [...Object.entries(showCard.via_counts ?? {})]
            .filter(([v]) => Number(v) > 0 && Number(v) !== 1 && Number(v) !== showCard.node_id && POS[Number(v)])
            .map(([v, c]) => {
              const a = POS[showCard.node_id]
              const b = POS[Number(v)]
              const below = b.y > a.y
              return (
                <text
                  key={v}
                  x={b.x + (a.x - b.x) * 0.38}
                  y={b.y + (a.y - b.y) * 0.38 + (below ? 22 : -12)}
                  textAnchor="middle"
                  className="m-via"
                >
                  {c} frags via {LETTER[Number(v)]}
                </text>
              )
            })}

        {/* USB link: bridge -> laptop */}
        <line x1={POS[1].x - 82} y1={LAPTOP.y} x2={LAPTOP.x + 52} y2={LAPTOP.y} className="m-usb" markerEnd="url(#arrow)" />
        <text x={(POS[1].x - 82 + LAPTOP.x + 52) / 2} y={LAPTOP.y - 10} className="m-small" textAnchor="middle">
          USB
        </text>

        {path && !reducedMotion && (
          <circle key={showCard.lastEventMs} r="6" className={sim ? 'm-packet is-sim' : 'm-packet'}>
            <animateMotion dur="0.7s" fill="freeze" path={path} />
          </circle>
        )}

        <g>
          <rect x={LAPTOP.x - 50} y={LAPTOP.y - 30} width="100" height="60" rx="6" className="m-laptop" />
          <text x={LAPTOP.x} y={LAPTOP.y - 2} textAnchor="middle" className="m-name">
            Dashboard
          </text>
          <text x={LAPTOP.x} y={LAPTOP.y + 15} textAnchor="middle" className="m-sub">
            laptop
          </text>
        </g>

        <g>
          <rect x={POS[1].x - 80} y={POS[1].y - 38} width="160" height="76" rx="7" className="m-bridge" />
          <text x={POS[1].x} y={POS[1].y - 12} textAnchor="middle" className="m-bridge-name">
            Node 1 · Bridge
          </text>
          <text x={POS[1].x} y={POS[1].y + 7} textAnchor="middle" className="m-bridge-sub">
            receives &amp; CRC-checks files
          </text>
          <text x={POS[1].x} y={POS[1].y + 22} textAnchor="middle" className="m-bridge-sub">
            does not make captures
          </text>
        </g>

        {[2, 3, 4, 5].map((id) => {
          const p = POS[id]
          const n = nodes.find((x) => x.node_id === id)
          const count = byNode?.[String(id)] ?? 0
          const recent = n?.last_seen != null && now - n.last_seen < 600
          const t = sending.get(id)
          const cls = t ? (t.is_demo ? ' is-sim-live' : ' is-live') : recent ? '' : ' is-stale'
          const labelY = id === 2 || id === 3 ? p.y - R - 14 : p.y + R + 20
          return (
            <g key={id}>
              <circle cx={p.x} cy={p.y} r={R} className={'m-node' + cls} />
              {t && !reducedMotion && <circle cx={p.x} cy={p.y} r={R} className="m-ring" />}
              <text x={p.x} y={p.y - 1} textAnchor="middle" className="m-name">
                Node {id}
              </text>
              <text x={p.x} y={p.y + 14} textAnchor="middle" className="m-sub">
                {LETTER[id]} · field
              </text>
              {count > 0 && (
                <g>
                  <circle cx={p.x + 24} cy={p.y - 24} r="12" className={mode === 'sim' ? 'm-badge is-sim' : 'm-badge'} />
                  <text x={p.x + 24} y={p.y - 20} textAnchor="middle" className="m-badge-t">
                    {count}
                  </text>
                </g>
              )}
              <text x={p.x} y={labelY} textAnchor="middle" className={t ? 'm-small is-blue' : 'm-small'}>
                {t
                  ? t.is_demo
                    ? 'simulated · sending'
                    : 'sending'
                  : mode === 'sim'
                    ? 'not used for status'
                    : n?.last_seen
                      ? `last capture ${ago(n.last_seen, now)}`
                      : 'no real captures yet'}
              </text>
            </g>
          )
        })}
      </svg>
      {showCard && <ArrivingCard t={showCard} />}
    </div>
  )
}

function ArrivingCard({ t }: { t: LiveTransfer }) {
  const total = t.frag_count ?? null
  const pct = total ? Math.min(100, (t.have / total) * 100) : 0
  const done = !!t.finishedAt
  const failed = t.status === 'failed'
  const kind = t.kind === 'telemetry' ? 'telemetry' : t.kind === 'image' ? 'image' : t.kind === 'test' ? 'test file' : 'file'
  const name =
    t.capture_id != null ? `capture ${captureName({ node_id: t.node_id, capture_id: t.capture_id, display_id: null })}` : `session ${t.sid ?? '?'}`
  const via = t.last_via
  return (
    <div className={'arriving' + (t.is_demo ? ' is-sim' : '') + (failed ? ' is-failed' : '')} role="status" aria-live="polite">
      <div className="arriving-head">
        <i className="dot is-blue" />
        {failed ? 'FAILED' : done ? 'VERIFIED' : 'ARRIVING NOW'} · Node {t.node_id} → Bridge
        {t.is_demo ? <span className="tag is-sim">SIMULATED</span> : null}
      </div>
      <div className="arriving-line">
        {radioName(t.radio)} · {name}
        {t.size_bytes != null && ` · ${size(t.size_bytes)} ${kind}`}
      </div>
      <div className="bar" aria-label="Fragments received">
        <span style={{ width: `${failed ? 100 : done ? 100 : pct}%` }} />
      </div>
      <div className="arriving-foot">
        {failed
          ? `Failed on the bridge: ${t.fail_reason ?? 'reason not reported'}`
          : `${t.have} / ${total ?? '?'} fragments`}
        {t.elapsed_s != null && !failed && ` · ${secs(t.elapsed_s)}`}
        {t.nacks ? ` · ${t.nacks} NACK${t.nacks === 1 ? '' : 's'}` : ''}
        {done && !failed && ' · bridge CRC ✓'}
      </div>
      <div className="arriving-note">
        {t.radio === 'lora'
          ? `LoRa: ${t.last_hops ? `${t.last_hops} hop${t.last_hops === 1 ? '' : 's'}, ` : ''}relay unknown on LoRa`
          : via
            ? `last via ${via === t.node_id || via === 1 ? 'direct' : LETTER[via]}${t.last_hops ? ` · ${t.last_hops} hop${t.last_hops === 1 ? '' : 's'}` : ''}`
            : 'waiting for the first fragment'}
      </div>
    </div>
  )
}
