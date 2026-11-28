import { soundName } from '../lib/format'
import type { SoundClassRow } from '../lib/types'
import { Note } from './ui'

export function SoundClassBars({ rows, limit }: { rows: SoundClassRow[]; limit?: number }) {
  const shown = limit ? rows.slice(0, limit) : rows
  const max = Math.max(1, ...rows.map((r) => r.count))
  if (!rows.length) return <div className="empty-line">No sound classes reported for these filters.</div>
  return (
    <div>
      <table className="tbl sound-tbl">
        <thead>
          <tr>
            <th>Sound class</th>
            <th>Captures</th>
            <th />
            <th>Avg confidence</th>
            <th>Reported by</th>
          </tr>
        </thead>
        <tbody>
          {shown.map((r) => (
            <tr key={r.sound_class}>
              <td>
                <strong>{soundName(r.sound_label)}</strong>
                {r.sound_group && r.sound_group !== r.sound_label && <span className="muted"> · {soundName(r.sound_group)}</span>}
              </td>
              <td className="num">{r.count}</td>
              <td className="bar-cell">
                <span className={'hbar r-amber' + (r.sim_count === r.count ? ' is-sim' : '')} style={{ width: `${(r.count / max) * 100}%` }} />
              </td>
              <td>
                {r.mean_conf != null ? (
                  <span className="conf">
                    <span className="conf-track">
                      <span style={{ width: `${r.mean_conf}%` }} />
                    </span>
                    {Math.round(r.mean_conf)}%
                  </span>
                ) : (
                  '—'
                )}
              </td>
              <td>{r.nodes.map((n) => `Node ${n}`).join(', ')}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <Note>Sound classes are the on-node classifier's output, not confirmed species.</Note>
    </div>
  )
}
