import { api } from '../lib/api'
import { useFilters, type DateRange } from '../lib/filters'
import { rangeLabel } from '../lib/format'
import type { Mode, RadioFilter } from '../lib/types'
import { Segmented } from './ui'

function toLocalInput(ts: number | null): string {
  if (ts == null) return ''
  const d = new Date(ts * 1000)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`
}

function fromLocalInput(v: string): number | null {
  if (!v) return null
  const t = new Date(v).getTime()
  return Number.isFinite(t) ? t / 1000 : null
}

export function FilterBar({ range }: { range: { first: number | null; last: number | null } | null }) {
  const { filters, setFilters } = useFilters()
  return (
    <div className="filterbar">
      <label className="fb-field">
        <span>Date</span>
        <select value={filters.date} onChange={(e) => setFilters({ date: e.target.value as DateRange })}>
          <option value="all">All data</option>
          <option value="today">Today, {new Date().toLocaleDateString('en-GB', { day: 'numeric', month: 'short' })}</option>
          <option value="hour">Last hour</option>
          <option value="custom">Custom…</option>
        </select>
      </label>
      {filters.date === 'custom' && (
        <span className="fb-custom">
          <input
            type="datetime-local"
            aria-label="From"
            value={toLocalInput(filters.from)}
            onChange={(e) => setFilters({ from: fromLocalInput(e.target.value) })}
          />
          –
          <input
            type="datetime-local"
            aria-label="To"
            value={toLocalInput(filters.to)}
            onChange={(e) => setFilters({ to: fromLocalInput(e.target.value) })}
          />
        </span>
      )}
      <span className="fb-label">Radio</span>
      <Segmented<RadioFilter | 'all'>
        label="Radio"
        value={filters.radio ?? 'all'}
        onChange={(v) => setFilters({ radio: v === 'all' ? null : v })}
        options={[
          { value: 'all', label: 'All' },
          { value: 'lora', label: 'LoRa' },
          { value: 'wifi', label: 'Wi-Fi' },
          { value: 'unknown', label: 'Unknown' },
        ]}
      />
      <span className="fb-label">Data</span>
      <Segmented<Mode>
        label="Data source"
        value={filters.mode}
        onChange={(v) => setFilters({ mode: v })}
        options={[
          { value: 'real', label: 'Real' },
          { value: 'sim', label: 'Simulated' },
          { value: 'both', label: 'Both' },
        ]}
      />
      <div className="fb-spacer" />
      <a className="btn btn-ghost" href={api.exportUrl(filters, 'csv')} download>
        ⬇ CSV
      </a>
      <a className="btn btn-ghost" href={api.exportUrl(filters, 'json')} download>
        ⬇ JSON
      </a>
      <span className="fb-range">
        Data available: <strong>{range ? rangeLabel(range.first, range.last) : '…'}</strong> · partial history
      </span>
    </div>
  )
}
