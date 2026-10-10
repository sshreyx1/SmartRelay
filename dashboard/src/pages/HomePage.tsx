import { Link } from 'react-router-dom'
import { CaptureFeed } from '../components/CaptureFeed'
import { EnvironmentChart } from '../components/EnvironmentChart'
import { LiveTransfers } from '../components/LiveTransfers'
import { MeshLog } from '../components/MeshLog'
import type { LiveMesh } from '../hooks/useLiveMesh'

const HOME_FEED_LIMIT = 6

export function HomePage({ live }: { live: LiveMesh }) {
  const { nodes, captures, log, activity } = live

  const peakHour = activity.reduce((best, b) => (b.count > best.count ? b : best), { hour: 0, count: 0 })

  return (
    <>
      <LiveTransfers />

      <CaptureFeed
        captures={captures}
        limit={HOME_FEED_LIMIT}
        footer={
          captures.length > HOME_FEED_LIMIT ? (
            <div className="feed-footer">
              <Link className="btn btn-ghost" to="/sightings">
                View all {captures.length} captures
              </Link>
            </div>
          ) : undefined
        }
      />

      <EnvironmentChart nodes={nodes} captures={captures} />

      <section className="section">
        <div className="section-head">
          <h2>Activity by hour</h2>
        </div>
        <div className="activity">
          <div className="activity-chart">
            {activity.map((bucket) => {
              const peak = Math.max(1, ...activity.map((b) => b.count))
              return (
                <div
                  key={bucket.hour}
                  className={'activity-bar' + (bucket.count > 0 ? ' has-count' : '')}
                  style={{ height: `${Math.max(2, (bucket.count / peak) * 100)}%` }}
                  title={`${String(bucket.hour).padStart(2, '0')}:00 — ${bucket.count} capture${bucket.count === 1 ? '' : 's'}`}
                />
              )
            })}
          </div>
          <div className="activity-axis">
            <span>00:00</span>
            <span>06:00</span>
            <span>12:00</span>
            <span>18:00</span>
            <span>23:00</span>
          </div>
          {peakHour.count > 0 ? (
            <div className="activity-note">
              Most activity around {String(peakHour.hour).padStart(2, '0')}:00, with {peakHour.count} capture
              {peakHour.count === 1 ? '' : 's'}.
            </div>
          ) : (
            <div className="activity-note">No real captures yet to chart.</div>
          )}
        </div>
      </section>

      <section className="section">
        <div className="section-head">
          <h2>Species seen</h2>
        </div>
        <div className="empty">
          <strong>TBD — no species classifier wired in yet</strong>
          Once captures are identified, a tally of what's been confirmed will show here, same as it
          did in the old UI's "Confirmed this week" panel.
        </div>
      </section>

      <MeshLog log={log} />
    </>
  )
}
