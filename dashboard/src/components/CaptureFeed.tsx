import { useState } from 'react'
import { num, timeAgo } from '../lib/api'
import type { Capture } from '../lib/types'
import { CaptureModal } from './CaptureModal'
import { IdBadge, SoundChip, SoundHeading } from './SoundLabel'

interface Props {
  captures: Capture[]
  limit?: number
  footer?: React.ReactNode
  emptyHint?: React.ReactNode
  title?: string
  showCount?: boolean
}

/**
 * Port of the old UI's SightingFeed, same card layout and CSS classes
 * (.sighting / .sighting-image / .confidence / .readings / .chip …). The
 * name on each card is the sound label the NODE decided (two-stage on-node
 * classifier, common/sound_classes.h) -- "Dog", or "Bird (unidentified)" when
 * the node only trusted the group -- with its confidence chip and the
 * capture's N2-000042 id badge. Sensor readings, trigger, radio and weather
 * are the real values from capture_meta.h.
 */
export function CaptureFeed({ captures, limit, footer, emptyHint, title = 'Recent captures', showCount = true }: Props) {
  const [openId, setOpenId] = useState<number | null>(null)
  const visible = limit ? captures.slice(0, limit) : captures
  const open = openId != null ? captures.find((c) => c.id === openId) : undefined

  return (
    <section className="section">
      {title && (
        <div className="section-head">
          <h2>{title}</h2>
          {showCount && captures.length > 0 && <span className="section-count">{captures.length}</span>}
        </div>
      )}

      {captures.length === 0 ? (
        emptyHint ?? (
          <div className="empty">
            <strong>Nothing captured yet</strong>
            Nodes send a photo when their motion and sound sensors both agree something moved.
            <br />
            <code>python3 simulator/demo_feed.py --burst 3</code>
          </div>
        )
      ) : (
        <>
          {visible.map((c) => (
            <CaptureCard key={c.id} capture={c} onOpen={() => setOpenId(c.id)} />
          ))}
          {footer}
        </>
      )}

      {open && <CaptureModal capture={open} onClose={() => setOpenId(null)} />}
    </section>
  )
}

function CaptureCard({ capture, onOpen }: { capture: Capture; onOpen: () => void }) {
  const readings = [
    capture.temperature_c != null ? { label: 'Temp', value: `${num(capture.temperature_c, 1)}°C` } : null,
    capture.humidity_pct != null ? { label: 'Humidity', value: `${num(capture.humidity_pct, 0)}%` } : null,
    capture.light_raw != null ? { label: 'Light', value: num(capture.light_raw, 0) } : null,
  ].filter(Boolean) as { label: string; value: string }[]

  const trigger = [capture.trigger_pir ? 'PIR' : null, capture.trigger_sound ? 'Sound' : null]
    .filter(Boolean)
    .join(' + ')

  const classes = ['sighting', 'is-clickable', capture.is_demo ? 'is-review' : ''].filter(Boolean).join(' ')

  return (
    <article
      className={classes}
      onClick={onOpen}
      role="button"
      tabIndex={0}
      onKeyDown={(e) => (e.key === 'Enter' || e.key === ' ') && onOpen()}
    >
      <div className="sighting-thumb">
        {capture.image_url ? (
          <img
            className="sighting-image"
            src={capture.image_url}
            alt={`Capture from node ${capture.node_id}`}
            loading="lazy"
          />
        ) : (
          <div className="sighting-image" style={{ display: 'grid', placeItems: 'center', fontSize: 11 }}>
            no image
          </div>
        )}
      </div>

      <div>
        <div className="sighting-head">
          <SoundHeading capture={capture} />
          <div className="sighting-when">
            <IdBadge capture={capture} /> {timeAgo(capture.received_at)}
          </div>
        </div>

        <div className="confidence">
          <SoundChip capture={capture} />
          <span className="chip is-node">Node {capture.node_id}</span>
          {trigger && <span className="chip">{trigger}</span>}
          {capture.radio && <span className="chip">{capture.radio}</span>}
          {capture.weather_name && <span className="chip">{capture.weather_name}</span>}
          <span className="chip is-confirmed">crc verified</span>
          {capture.is_demo === 1 && <span className="chip is-review">simulated</span>}
        </div>

        {readings.length > 0 && (
          <div className="readings">
            {readings.map((r) => (
              <div className="reading" key={r.label}>
                {r.label}
                <strong>{r.value}</strong>
              </div>
            ))}
          </div>
        )}
      </div>
    </article>
  )
}
