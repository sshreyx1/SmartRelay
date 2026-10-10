import { useMemo, useState } from 'react'
import { num, timeAgo } from '../lib/api'
import { Sparkline } from './Sparkline'
import type { Capture, MeshNode } from '../lib/types'

interface Props {
  nodes: MeshNode[]
  captures: Capture[]
}

interface Metric {
  key: 'temperature_c' | 'humidity_pct' | 'light_raw'
  label: string
  unit: string
  color: string
  fill: string
  digits: number
}

const METRICS: Metric[] = [
  { key: 'temperature_c', label: 'Temperature', unit: '°C', color: 'var(--detect)', fill: 'var(--detect-soft)', digits: 1 },
  { key: 'humidity_pct', label: 'Humidity', unit: '%', color: 'var(--signal)', fill: 'var(--signal-soft)', digits: 0 },
  { key: 'light_raw', label: 'Light (raw)', unit: '', color: 'var(--forest)', fill: 'var(--forest-soft)', digits: 0 },
]

/**
 * Trend, not just the latest reading -- same intent as the old UI's
 * environment chart, but the series is derived client-side from the real
 * captures already on hand rather than fetched from a dedicated telemetry
 * endpoint (the backend doesn't log a reading except alongside a capture).
 * There's no UV sensor in the real sensor stack, so that metric is dropped
 * rather than shown as TBD on every card.
 */
export function EnvironmentChart({ nodes, captures }: Props) {
  const reportingNodes = useMemo(() => nodes.filter((n) => n.status !== 'never seen'), [nodes])
  const [selected, setSelected] = useState<number | null>(null)
  const activeNode = selected ?? reportingNodes[0]?.node_id ?? null

  const series = useMemo(() => {
    if (activeNode == null) return []
    return captures
      .filter((c) => c.node_id === activeNode && !c.is_demo)
      .slice()
      .sort((a, b) => a.received_at - b.received_at)
  }, [captures, activeNode])

  if (nodes.length === 0) return null

  return (
    <section className="section">
      <div className="section-head">
        <h2>Environmental conditions</h2>
      </div>

      <div className="env-panel">
        <div className="env-tabs">
          {reportingNodes.map((n) => (
            <button
              key={n.node_id}
              className={'env-tab' + (n.node_id === activeNode ? ' is-active' : '')}
              onClick={() => setSelected(n.node_id)}
            >
              Node {n.node_id}
            </button>
          ))}
        </div>

        {series.length < 2 ? (
          <div className="env-empty">
            Not enough real captures yet from this node to chart a trend. Readings fill in as
            captures arrive (demo/simulated readings are excluded here).
          </div>
        ) : (
          <div className="env-grid">
            {METRICS.map((metric) => (
              <MetricCard key={metric.key} metric={metric} series={series} />
            ))}
          </div>
        )}
      </div>
    </section>
  )
}

function MetricCard({ metric, series }: { metric: Metric; series: Capture[] }) {
  const values = series.map((c) => c[metric.key])
  const latest = [...values].reverse().find((v) => v != null) ?? null
  const latestPoint = [...series].reverse().find((c) => c[metric.key] != null)

  return (
    <div className="env-card">
      <div className="env-card-head">
        <span className="env-card-label">{metric.label}</span>
        <span className="env-card-value">
          {num(latest, metric.digits)}
          {metric.unit && latest != null && <small>{metric.unit}</small>}
        </span>
      </div>
      <Sparkline values={values} height={44} color={metric.color} fillColor={metric.fill} />
      {latestPoint && <div className="env-card-time">updated {timeAgo(latestPoint.received_at)}</div>}
    </div>
  )
}
