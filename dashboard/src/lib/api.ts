import type {
  BridgeDetail,
  Capture,
  FieldNodeDetail,
  HealthInfo,
  LogLine,
  MeshNode,
  SoundClassRow,
  Summary,
  Transfer,
} from './types'
import type { FilterState } from './filters'

/** REST client for backend/app.py. The 2-second poll (hooks/usePolling.ts) is the source of truth;
 *  the WebSocket only makes things appear sooner. */

async function getJSON<T>(path: string, signal?: AbortSignal): Promise<T> {
  const res = await fetch(path, { signal })
  if (!res.ok) throw new Error(`${path} returned ${res.status}`)
  return (await res.json()) as T
}

/** Filter bar -> backend query string (node_id, radio, since, until, mode). */
export function filterParams(f: FilterState, extra: Record<string, string | number | undefined> = {}): string {
  const p = new URLSearchParams()
  p.set('mode', f.mode)
  if (f.node != null) p.set('node_id', String(f.node))
  if (f.radio) p.set('radio', f.radio)
  if (f.trigger) p.set('trigger', f.trigger)
  if (f.q) p.set('q', f.q)
  const { since, until } = dateWindow(f)
  if (since != null) p.set('since', String(since))
  if (until != null) p.set('until', String(until))
  for (const [k, v] of Object.entries(extra)) if (v !== undefined) p.set(k, String(v))
  return p.toString()
}

/** The date filter as epoch seconds. "today" and "hour" are re-evaluated on every poll. */
export function dateWindow(f: FilterState): { since?: number; until?: number } {
  const now = Date.now() / 1000
  if (f.date === 'hour') return { since: now - 3600 }
  if (f.date === 'today') {
    const d = new Date()
    d.setHours(0, 0, 0, 0)
    return { since: d.getTime() / 1000 }
  }
  if (f.date === 'custom') return { since: f.from ?? undefined, until: f.to ?? undefined }
  return {}
}

export const api = {
  health: (signal?: AbortSignal) => getJSON<HealthInfo>('/api/health', signal),
  nodes: (signal?: AbortSignal) => getJSON<{ nodes: MeshNode[] }>('/api/nodes', signal).then((r) => r.nodes),
  summary: (f: FilterState, signal?: AbortSignal) => getJSON<Summary>(`/api/summary?${filterParams(f)}`, signal),
  captures: (f: FilterState, limit = 20, signal?: AbortSignal) =>
    getJSON<{ captures: Capture[]; total: number }>(`/api/captures?${filterParams(f, { limit })}`, signal),
  images: (f: FilterState, limit = 20, signal?: AbortSignal) =>
    getJSON<{ captures: Capture[]; total: number }>(
      `/api/captures?${filterParams(f, { limit, has_image: 'true' })}`,
      signal,
    ),
  capture: (id: number, signal?: AbortSignal) => getJSON<Capture>(`/api/captures/${id}`, signal),
  inFlight: (signal?: AbortSignal) =>
    getJSON<{ transfers: Transfer[] }>('/api/transfers/in_flight', signal).then((r) => r.transfers),
  fieldNode: (id: number, f: FilterState, signal?: AbortSignal) =>
    getJSON<FieldNodeDetail>(`/api/nodes/${id}?${filterParams({ ...f, node: null })}`, signal),
  bridge: (signal?: AbortSignal) => getJSON<BridgeDetail>('/api/nodes/1', signal),
  soundClasses: (f: FilterState, signal?: AbortSignal) =>
    getJSON<{ classes: SoundClassRow[] }>(`/api/sound_classes?${filterParams(f)}`, signal).then((r) => r.classes),
  log: (opts: { limit?: number; beforeId?: number; kind?: string; showEvt?: boolean }, signal?: AbortSignal) => {
    const p = new URLSearchParams({ limit: String(opts.limit ?? 20), kind: opts.kind ?? 'all' })
    if (opts.beforeId != null) p.set('before_id', String(opts.beforeId))
    if (opts.showEvt) p.set('show_evt', 'true')
    return getJSON<{ lines: LogLine[] }>(`/api/log?${p}`, signal).then((r) => r.lines)
  },
  exportUrl: (f: FilterState, format: 'csv' | 'json') => `/api/captures/export?${filterParams(f, { format })}`,
}
