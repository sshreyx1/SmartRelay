import type { LogLine } from '../lib/types'

/**
 * The raw lines the bridge sends over its serial uplink, shown as-is --
 * this is the actual text (common/uplink.h), not a reformatted summary.
 */
export function MeshLog({ log }: { log: LogLine[] }) {
  return (
    <section className="section">
      <div className="section-head">
        <h2>Mesh log</h2>
      </div>
      <div className="log">
        {log.length === 0 ? (
          <div className="log-row">
            <span className="log-text">Waiting for mesh activity…</span>
          </div>
        ) : (
          log
            .slice()
            .reverse()
            .map((entry) => (
              <div className="log-row" key={entry.id}>
                <span className="log-time">
                  {new Date(entry.received_at * 1000).toLocaleTimeString('en-GB', { hour12: false })}
                </span>
                <span className="log-text">{entry.line}</span>
              </div>
            ))
        )}
      </div>
    </section>
  )
}
