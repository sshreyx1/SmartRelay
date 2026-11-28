import { useState } from 'react'
import { api } from '../lib/api'
import { useFilters } from '../lib/filters'
import { CaptureTable } from '../components/CaptureTable'
import { Insights } from '../components/Insights'
import { Kpi } from '../components/Kpi'
import { LatestCaptures } from '../components/LatestCaptures'
import { MeshDiagram } from '../components/MeshDiagram'
import { RadioMix } from '../components/RadioMix'
import { SoundClassBars } from '../components/SoundClassBars'
import { Timeline } from '../components/Timeline'
import { Note, Panel } from '../components/ui'
import { usePolling } from '../hooks/usePolling'
import { useSummary } from '../state/SummaryContext'

type Tab = 'timeline' | 'radio' | 'sound' | 'records'

export function OverviewPage() {
  const { filters, key } = useFilters()
  const { data: s, error } = useSummary()
  const latest = usePolling((sig) => api.captures(filters, 20, sig), key)
  const [tab, setTab] = useState<Tab>('timeline')
  const sim = filters.mode === 'sim'

  return (
    <div className="overview">
      {error && !s && <div className="error-line">Backend not reachable: {error}</div>}
      <div className="kpis">
        <Kpi
          hero
          label="Real captures"
          value={sim ? null : s?.real_captures ?? null}
          unavailable={
            sim ? (
              <div className="kpi-value is-text">Not in this view</div>
            ) : undefined
          }
          sub="Verified field captures, excluding simulations."
          side={
            sim ? (
              <span className="kpi-hint">
                Data filter is Simulated
                <br />
                Switch to Real or Both
              </span>
            ) : (
              s && (
                <ul className="kpi-split">
                  {[2, 3, 4, 5].map((n) => (
                    <li key={n}>
                      Node {n} · {s.by_node[String(n)] ?? 0}
                    </li>
                  ))}
                </ul>
              )
            )
          }
        />
        <Kpi
          label="Dual-sensor captures"
          value={s?.dual_sensor.count ?? null}
          of={s?.dual_sensor.of}
          sub="Captures activated by both sensors."
          tag={sim ? <span className="tag is-sim kpi-tag">SIM</span> : undefined}
        />
        <Kpi
          label="CRC-verified transfers"
          value={s?.crc_verified_transfers ?? null}
          sub="Complete files accepted by bridge."
          tag={sim ? <span className="tag is-sim kpi-tag">SIM</span> : undefined}
        />
        <Kpi
          label="Field nodes reporting"
          value={s?.field_nodes_reporting?.count ?? null}
          of={s?.field_nodes_reporting?.of}
          unavailable={s && s.field_nodes_reporting == null ? <div className="kpi-value is-text">Data Unavailable</div> : undefined}
          side={s && s.field_nodes_reporting == null ? <span className="kpi-hint">Status uses real captures only</span> : undefined}
          sub="Field nodes with recent captures."
        />
      </div>

      <div className="ov-grid">
        <div className="ov-main">
          <Panel
            title="How a capture reaches the dashboard"
            right={
              <span className="legend">
                <span className="tag is-capture">capture</span> <i className="ln" /> radio neighbours <i className="dot is-blue" /> arriving file
              </span>
            }
          >
            <MeshDiagram byNode={s?.by_node ?? null} mode={filters.mode} />
            <Note>
              Grey lines: which nodes can hear each other. Blue: the relay each fragment came through, as reported
              by the bridge (Wi-Fi only; LoRa reports hop count only). "Reporting" means a capture arrived in the
              last 10 minutes, not a live heartbeat.
            </Note>
          </Panel>

          <section className="panel tabs-panel">
            <div className="subtabs" role="tablist">
              {(
                [
                  ['timeline', 'Capture timeline'],
                  ['radio', 'Radio mix'],
                  ['sound', 'Sound classes'],
                  ['records', 'Capture records'],
                ] as [Tab, string][]
              ).map(([id, label]) => (
                <button key={id} role="tab" aria-selected={tab === id} className={tab === id ? 'is-on' : ''} onClick={() => setTab(id)}>
                  {label}
                </button>
              ))}
            </div>
            <div className="subtab-body">
              {!s ? (
                <div className="skeleton" />
              ) : tab === 'timeline' ? (
                <Timeline summary={s} mode={filters.mode} />
              ) : tab === 'radio' ? (
                <RadioMix summary={s} />
              ) : tab === 'sound' ? (
                <SoundClassBars rows={s.sound_classes} />
              ) : (
                <CaptureTable />
              )}
            </div>
          </section>
        </div>

        <aside className="ov-side">
          <Insights items={s?.insights ?? null} />
          <LatestCaptures captures={latest.data?.captures ?? null} />
        </aside>
      </div>
    </div>
  )
}
