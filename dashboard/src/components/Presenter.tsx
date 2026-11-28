import { useEffect, useState } from 'react'
import { api } from '../lib/api'
import { captureName, clock, imageFile, radioName, routeLabel, size, soundName } from '../lib/format'
import { useFilters } from '../lib/filters'
import { usePolling } from '../hooks/usePolling'
import { useNow } from '../hooks/useNow'
import { useLive } from '../state/LiveContext'
import { useSummary } from '../state/SummaryContext'
import { MeshDiagram } from './MeshDiagram'
import { SourceTag } from './ui'

const STEPS = ['Trigger', 'Capture', 'Radio transfer', 'Bridge verifies', 'Dashboard']
const RECENT_CAPTURE_S = 15

/** Full-screen view for the demo: big type, no filters or exports, only panels that have data. */
export function Presenter({ onClose }: { onClose: () => void }) {
  const { filters, key } = useFilters()
  const { arriving } = useLive()
  const { data: s } = useSummary()
  const now = useNow()
  const [dark, setDark] = useState(false)
  const latest = usePolling((sig) => api.images(filters, 1, sig), `presenter|${key}`)
  const last = latest.data?.captures[0]

  useEffect(() => {
    const el = document.documentElement
    if (!document.fullscreenElement) el.requestFullscreen?.().catch(() => {})
    const onFs = () => {
      if (!document.fullscreenElement) onClose()
    }
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose()
    document.addEventListener('fullscreenchange', onFs)
    window.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('fullscreenchange', onFs)
      window.removeEventListener('keydown', onKey)
      if (document.fullscreenElement) void document.exitFullscreen().catch(() => {})
    }
  }, [onClose])

  // Each step lights up from the real event that drives it.
  const t = arriving[arriving.length - 1]
  const recentCapture = last && now - last.received_at < RECENT_CAPTURE_S
  let lit = 0
  if (t) lit = t.finishedAt ? 4 : 3
  else if (recentCapture) lit = 5
  const sim = t ? !!t.is_demo : last ? !!last.is_demo : filters.mode === 'sim'
  const img = last ? imageFile(last) : undefined

  return (
    <div className={'presenter' + (dark ? ' is-dark' : '')}>
      <header className="p-head">
        <div className="brand">
          SmartRelay <span>wildlife trail mesh · live</span>
        </div>
        {sim && <span className="p-sim">SIMULATED</span>}
        <div className="p-actions">
          <label className="check">
            <input type="checkbox" checked={dark} onChange={(e) => setDark(e.target.checked)} /> dark
          </label>
          <button className="btn btn-ghost" onClick={onClose}>
            Exit (Esc)
          </button>
        </div>
      </header>

      <ol className="p-steps">
        {STEPS.map((label, i) => (
          <li key={label} className={i < lit ? 'is-lit' : ''}>
            <span>{i + 1}</span>
            {label}
          </li>
        ))}
      </ol>

      <div className="p-body">
        <div className="p-mesh">
          <MeshDiagram byNode={s?.by_node ?? null} mode={filters.mode} compact />
        </div>
        {last && (
          <figure className="p-latest">
            {last.image_url && <img src={last.image_url} alt="Latest image received" />}
            <figcaption>
              <div>
                <SourceTag demo={last.is_demo} /> <strong>Node {last.node_id}</strong> · {captureName(last)} ·{' '}
                {clock(last.received_at)}
              </div>
              <div>
                sound class <strong>{soundName(last.sound_label)}</strong>
                {last.sound_conf != null && ` ${last.sound_conf}%`} (not a confirmed species)
              </div>
              {img && (
                <div>
                  {size(img.size_bytes)} image in {img.frag_count ?? '?'} fragments via {radioName(img.radio)} ·{' '}
                  {img.crc_laptop_ok === 1 ? 'CRC-32 match ✓' : 'CRC not rechecked'}
                  {img.radio_s != null ? ` · ${img.radio_s.toFixed(1)} s` : ''}
                  {routeLabel(img) ? ` · ${routeLabel(img)}` : ''}
                  {img.nacks ? ` · ${img.nacks} NACK` : ''}
                </div>
              )}
            </figcaption>
          </figure>
        )}
      </div>

      <footer className="p-strip">
        {sim ? 'Scenario: simulated demo feed (started from the terminal)' : 'Live data from the bridge (Node 1) over USB'}
      </footer>
    </div>
  )
}
