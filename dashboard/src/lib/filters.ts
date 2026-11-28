import { useCallback, useMemo } from 'react'
import { useLocation, useSearchParams } from 'react-router-dom'
import type { Mode, RadioFilter } from './types'

/** The filter bar's state. It lives in the URL query so a view can be shared or reloaded as-is. */
export type DateRange = 'all' | 'today' | 'hour' | 'custom'
export type TriggerFilter = 'dual' | 'pir' | 'sound' | 'manual'

export interface FilterState {
  date: DateRange
  from: number | null
  to: number | null
  radio: RadioFilter | null
  mode: Mode
  node: number | null
  trigger: TriggerFilter | null
  q: string | null
}

const DATES: DateRange[] = ['all', 'today', 'hour', 'custom']
const RADIOS: RadioFilter[] = ['lora', 'wifi', 'unknown']
const MODES: Mode[] = ['real', 'sim', 'both']
const TRIGGERS: TriggerFilter[] = ['dual', 'pir', 'sound', 'manual']

function pick<T extends string>(v: string | null, allowed: T[]): T | null {
  return v && (allowed as string[]).includes(v) ? (v as T) : null
}

function numOrNull(v: string | null): number | null {
  if (v == null || v === '') return null
  const n = Number(v)
  return Number.isFinite(n) ? n : null
}

export function parseFilters(sp: URLSearchParams): FilterState {
  const node = numOrNull(sp.get('node'))
  return {
    date: pick(sp.get('date'), DATES) ?? 'all',
    from: numOrNull(sp.get('from')),
    to: numOrNull(sp.get('to')),
    radio: pick(sp.get('radio'), RADIOS),
    mode: pick(sp.get('data'), MODES) ?? 'real',
    node: node != null && node >= 2 && node <= 5 ? node : null,
    trigger: pick(sp.get('trigger'), TRIGGERS),
    q: sp.get('q') || null,
  }
}

const KEYS: Record<keyof FilterState, string> = {
  date: 'date',
  from: 'from',
  to: 'to',
  radio: 'radio',
  mode: 'data',
  node: 'node',
  trigger: 'trigger',
  q: 'q',
}
const DEFAULTS: Partial<Record<keyof FilterState, string>> = { date: 'all', mode: 'real' }

export function useFilters() {
  const [sp, setSp] = useSearchParams()
  const filters = useMemo(() => parseFilters(sp), [sp])
  const setFilters = useCallback(
    (patch: Partial<FilterState>) => {
      setSp(
        (prev) => {
          const next = new URLSearchParams(prev)
          for (const [k, v] of Object.entries(patch) as [keyof FilterState, unknown][]) {
            const key = KEYS[k]
            if (v == null || v === '' || String(v) === DEFAULTS[k]) next.delete(key)
            else next.set(key, String(v))
          }
          return next
        },
        { replace: true },
      )
    },
    [setSp],
  )
  /** A stable string that changes whenever any filter does (for effect/poll dependencies). */
  const key = sp.toString()
  return { filters, setFilters, key }
}

/** Keep the filter query when moving between pages. */
export function useHref() {
  const { search } = useLocation()
  return useCallback((path: string) => path + search, [search])
}
