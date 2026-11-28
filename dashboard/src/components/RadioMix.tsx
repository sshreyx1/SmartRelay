import { size } from '../lib/format'
import type { Summary } from '../lib/types'
import { Note, RadioTag } from './ui'

/** Completed files by radio and by node. Never a success rate: the bridge only reports files that arrived. */
export function RadioMix({ summary }: { summary: Summary }) {
  const rows = summary.radio_mix.rows
  const byRadio = new Map<string, { files: number; bytes: number; sizeN: number; frags: number; fragN: number; kinds: Record<string, number> }>()
  const byNode = new Map<number, Record<string, number>>()
  for (const r of rows) {
    const a = byRadio.get(r.radio) ?? { files: 0, bytes: 0, sizeN: 0, frags: 0, fragN: 0, kinds: {} }
    a.files += r.files
    if (r.avg_size != null) {
      a.bytes += r.avg_size * r.files
      a.sizeN += r.files
    }
    if (r.avg_frags != null) {
      a.frags += r.avg_frags * r.files
      a.fragN += r.files
    }
    a.kinds[r.kind] = (a.kinds[r.kind] ?? 0) + r.files
    byRadio.set(r.radio, a)
    const n = byNode.get(r.node_id) ?? {}
    n[r.radio] = (n[r.radio] ?? 0) + r.files
    byNode.set(r.node_id, n)
  }
  const max = Math.max(1, ...[...byRadio.values()].map((v) => v.files))
  if (!rows.length) return <div className="empty-line">No verified transfers match these filters.</div>
  return (
    <div className="radiomix">
      <table className="tbl">
        <thead>
          <tr>
            <th>Radio</th>
            <th>Completed files</th>
            <th />
            <th>Avg size</th>
            <th>Avg fragments</th>
            <th>File types</th>
          </tr>
        </thead>
        <tbody>
          {['lora', 'wifi', 'unknown'].filter((r) => byRadio.has(r)).map((r) => {
            const a = byRadio.get(r)!
            return (
              <tr key={r}>
                <td>
                  <RadioTag radio={r} />
                </td>
                <td className="num">{a.files}</td>
                <td className="bar-cell">
                  <span className={`hbar r-${r}`} style={{ width: `${(a.files / max) * 100}%` }} />
                </td>
                <td>{a.sizeN ? size(a.bytes / a.sizeN) : '—'}</td>
                <td>{a.fragN ? (a.frags / a.fragN).toFixed(1) : <span className="muted">not in bridge log</span>}</td>
                <td className="muted">
                  {Object.entries(a.kinds)
                    .map(([k, v]) => `${v} ${k === 'unknown' ? 'type not recorded' : k}`)
                    .join(' · ')}
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>
      <table className="tbl is-compact">
        <thead>
          <tr>
            <th>Node</th>
            <th>LoRa files</th>
            <th>Wi-Fi files</th>
          </tr>
        </thead>
        <tbody>
          {[...byNode.entries()].sort((a, b) => a[0] - b[0]).map(([n, v]) => (
            <tr key={n}>
              <td>Node {n}</td>
              <td className="num">{v.lora ?? 0}</td>
              <td className="num">{v.wifi ?? 0}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <Note>Counts of completed files, not success rates. The bridge only reports files that arrived whole.</Note>
    </div>
  )
}
