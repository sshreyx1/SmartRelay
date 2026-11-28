import { Bar, BarChart, CartesianGrid, ReferenceArea, ResponsiveContainer, Tooltip, XAxis, YAxis } from 'recharts'
import { clock } from '../lib/format'
import type { Mode, Summary } from '../lib/types'

interface Row {
  t: number
  real: number
  sim: number
  noData: boolean
}

function binLabel(t: number, binS: number): string {
  if (binS >= 86400) return new Date(t * 1000).toLocaleDateString('en-GB', { day: 'numeric', month: 'short' })
  if (binS >= 6 * 3600)
    return new Date(t * 1000).toLocaleString('en-GB', { day: 'numeric', month: 'short', hour: '2-digit', hour12: false }) + 'h'
  return clock(t, false)
}

export function Timeline({ summary, mode }: { summary: Summary; mode: Mode }) {
  const { bin_s, bins } = summary.timeline
  const first = summary.range.first
  const rows: Row[] = bins.map((b) => ({ ...b, noData: first == null || b.t + bin_s <= first }))
  const noDataEnd = rows.findIndex((r) => !r.noData)
  const unit = bin_s === 900 ? '15 minutes' : bin_s === 3600 ? 'hour' : bin_s === 21600 ? '6 hours' : 'day'
  const what = mode === 'sim' ? 'Simulated captures' : mode === 'both' ? 'Captures (real and simulated)' : 'Real captures'

  if (!rows.length) return <div className="empty-line">No captures match these filters.</div>

  return (
    <div className="timeline">
      <div className="chart-title">
        <span>
          {what} per {unit} · hover a bar for times and counts
        </span>
        {mode !== 'real' && (
          <span className="legend">
            <i className="sw is-real" /> real <i className="sw is-sim" /> simulated (hatched)
          </span>
        )}
      </div>
      <ResponsiveContainer width="100%" height={190}>
        <BarChart data={rows} margin={{ top: 8, right: 8, bottom: 0, left: -18 }} barCategoryGap={4}>
          <defs>
            <pattern id="hatch" width="6" height="6" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
              <rect width="6" height="6" fill="var(--sim-bg)" />
              <line x1="0" y1="0" x2="0" y2="6" stroke="var(--sim)" strokeWidth="2" />
            </pattern>
          </defs>
          <CartesianGrid vertical={false} stroke="var(--grid)" />
          <XAxis
            dataKey="t"
            tickFormatter={(t: number) => binLabel(t, bin_s)}
            tick={{ fontSize: 11, fill: 'var(--text-3)' }}
            tickLine={false}
            axisLine={{ stroke: 'var(--line)' }}
            minTickGap={28}
          />
          <YAxis allowDecimals={false} tick={{ fontSize: 11, fill: 'var(--text-3)' }} tickLine={false} axisLine={false} />
          {noDataEnd > 0 && (
            <ReferenceArea
              x1={rows[0].t}
              x2={rows[noDataEnd - 1].t}
              fill="var(--surface-2)"
              label={{ value: 'no data', fontSize: 10, fill: 'var(--text-3)' }}
            />
          )}
          <Tooltip
            cursor={{ fill: 'var(--hover)' }}
            content={({ active, payload }) => {
              if (!active || !payload?.length) return null
              const r = payload[0].payload as Row
              return (
                <div className="tip">
                  <strong>
                    {binLabel(r.t, bin_s)}–{clock(r.t + bin_s, false)}
                  </strong>
                  {r.noData ? (
                    <div>before the data range: no data</div>
                  ) : (
                    <>
                      {mode !== 'sim' && <div>{r.real} real captures</div>}
                      {mode !== 'real' && <div>{r.sim} simulated captures</div>}
                    </>
                  )}
                </div>
              )
            }}
          />
          {mode !== 'sim' && <Bar dataKey="real" stackId="a" fill="var(--amber)" radius={[3, 3, 0, 0]} isAnimationActive={false} />}
          {mode !== 'real' && (
            <Bar dataKey="sim" stackId="a" fill="url(#hatch)" stroke="var(--sim)" strokeDasharray="3 2" isAnimationActive={false} />
          )}
        </BarChart>
      </ResponsiveContainer>
    </div>
  )
}
