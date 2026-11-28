import { useEffect, useRef } from 'react'
import { bytes, num, timeAgo } from '../lib/api'
import type { Capture } from '../lib/types'
import { IdBadge, SoundChip, SoundHeading, isGroupOnly, prettyLabel } from './SoundLabel'

interface Props {
  capture: Capture
  onClose: () => void
}

/**
 * Port of the old UI's SightingModal. The sound label, group and confidence
 * come from the node's own two-stage classifier (decided on the Pico, only
 * displayed here). The review/flag workflow is still TBD (docs/SENSORS.md).
 * Everything else -- image, real sensor readings, trigger, transfer facts --
 * is the real row from the backend.
 */
export function CaptureModal({ capture, onClose }: Props) {
  const dialogRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose()
    }
    window.addEventListener('keydown', onKey)
    dialogRef.current?.focus()
    return () => window.removeEventListener('keydown', onKey)
  }, [onClose])

  const trigger = [capture.trigger_pir ? 'PIR' : null, capture.trigger_sound ? 'Sound' : null]
    .filter(Boolean)
    .join(' + ')

  const readings = [
    capture.temperature_c != null ? { label: 'Temperature', value: `${num(capture.temperature_c, 1)}°C` } : null,
    capture.humidity_pct != null ? { label: 'Humidity', value: `${num(capture.humidity_pct, 0)}%` } : null,
    capture.light_raw != null ? { label: 'Light (raw)', value: num(capture.light_raw, 0) } : null,
  ].filter(Boolean) as { label: string; value: string }[]

  return (
    <div className="modal-backdrop" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div
        className="modal"
        role="dialog"
        aria-modal="true"
        aria-label={`Capture detail: node ${capture.node_id}`}
        tabIndex={-1}
        ref={dialogRef}
      >
        <button className="modal-close" onClick={onClose} aria-label="Close">
          <CloseIcon />
        </button>

        <div className="modal-body">
          {capture.image_url ? (
            <img className="modal-image" src={capture.image_url} alt={`Capture from node ${capture.node_id}`} />
          ) : (
            <div className="modal-image" style={{ display: 'grid', placeItems: 'center', minHeight: 160 }}>
              no image
            </div>
          )}

          <div className="modal-detail">
            <div className="modal-head">
              <SoundHeading capture={capture} size={21} />
              <div className="sighting-when">{timeAgo(capture.received_at)}</div>
            </div>
            <div style={{ marginTop: 8 }}>
              <IdBadge capture={capture} large />
            </div>

            <div className="confidence" style={{ marginTop: 12 }}>
              <SoundChip capture={capture} />
              <span className="chip is-node">Node {capture.node_id}</span>
              {trigger && <span className="chip">{trigger}</span>}
              {capture.radio && <span className="chip">{capture.radio}</span>}
              {capture.weather_name && <span className="chip">{capture.weather_name}</span>}
              <span className="chip is-confirmed">crc verified</span>
              {capture.is_demo === 1 && <span className="chip is-review">simulated — not real mesh data</span>}
            </div>

            {readings.length > 0 && (
              <div className="readings" style={{ marginTop: 14 }}>
                {readings.map((r) => (
                  <div className="reading" key={r.label}>
                    {r.label}
                    <strong>{r.value}</strong>
                  </div>
                ))}
              </div>
            )}

            <div className="modal-alts">
              <div className="modal-section-label">Identified on-node</div>
              {capture.sound_label ? (
                <div className="modal-transfer-grid">
                  <div>
                    <span>{prettyLabel(capture.sound_label)}</span>
                    {isGroupOnly(capture.sound_label) ? 'group only' : 'label'}
                  </div>
                  <div>
                    <span>{prettyLabel(capture.sound_group)}</span>group
                  </div>
                  <div>
                    <span>{capture.sound_conf != null ? `${capture.sound_conf}%` : '—'}</span>confidence
                  </div>
                  <div>
                    <span>{num(capture.sound_rms, 0)}</span>sound RMS
                  </div>
                  <div>
                    <span>{num(capture.sound_zcr, 0)}</span>crossings / s
                  </div>
                  <div>
                    <span>{capture.image_name ?? '—'}</span>image the node picked
                  </div>
                </div>
              ) : (
                <div style={{ fontSize: 12.5, color: 'var(--text-3)' }}>
                  The node sent no sound label for this capture (recording failed, or firmware from
                  before the on-node classifier).
                </div>
              )}
              <div style={{ fontSize: 11.5, color: 'var(--text-3)', marginTop: 8 }}>
                Decided on the Pico by its two-stage tree classifier (sound group, then animal, with
                a confidence threshold). The base station only displays it.
              </div>
            </div>

            <div className="modal-transfer">
              <div className="modal-section-label">Transfer</div>
              <div className="modal-transfer-grid">
                <div>
                  <span>{capture.frag_total ?? '—'}</span>fragments
                </div>
                <div>
                  <span>{bytes(capture.size_bytes)}</span>size
                </div>
                <div>
                  <span>{capture.crc32 ?? '—'}</span>crc32
                </div>
                <div>
                  <span>TBD</span>duplicates
                </div>
                <div>
                  <span>TBD</span>retransmit rounds
                </div>
                <div>
                  <span>TBD</span>avg RSSI / SNR
                </div>
              </div>
            </div>

            <div className="review-note">
              Review workflow — TBD (flag / correct-label panel comes with the audio clips, docs/SENSORS.md
              B20).
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}

const CloseIcon = () => (
  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
    <path d="M18 6 6 18M6 6l12 12" />
  </svg>
)
