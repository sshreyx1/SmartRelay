import type { Capture, HealthInfo, LogLine, MeshNode } from './types'

/** REST helpers for the real backend. Most data arrives over the WebSocket
 *  (see hooks/useLiveMesh.ts); these are for the initial load and for
 *  paging through older captures. */

async function getJSON<T>(path: string): Promise<T> {
  const res = await fetch(path)
  if (!res.ok) throw new Error(`${path} returned ${res.status}`)
  return (await res.json()) as T
}

export interface CapturesQuery {
  limit?: number
  nodeId?: number
  includeDemo?: boolean
}

export const api = {
  health: () => getJSON<HealthInfo>('/api/health'),

  nodes: () => getJSON<{ nodes: MeshNode[]; note: string }>('/api/nodes').then((r) => r.nodes),

  captures: (query: CapturesQuery = {}) => {
    const params = new URLSearchParams()
    params.set('limit', String(query.limit ?? 50))
    if (query.nodeId != null) params.set('node_id', String(query.nodeId))
    if (query.includeDemo === false) params.set('include_demo', 'false')
    return getJSON<{ captures: Capture[] }>(`/api/captures?${params}`).then((r) => r.captures)
  },

  capture: (id: number) => getJSON<Capture>(`/api/captures/${id}`),

  log: (limit = 200) => getJSON<{ lines: LogLine[] }>(`/api/log?limit=${limit}`).then((r) => r.lines),
}

// --- formatting ------------------------------------------------------------

/** Relative time, e.g. "42s ago". Kept short -- these sit in tight columns. */
export function timeAgo(seconds?: number | null): string {
  if (!seconds) return '—'
  const delta = Math.max(0, Math.floor(Date.now() / 1000 - seconds))
  if (delta < 60) return `${delta}s ago`
  if (delta < 3600) return `${Math.floor(delta / 60)}m ago`
  if (delta < 86400) return `${Math.floor(delta / 3600)}h ago`
  return `${Math.floor(delta / 86400)}d ago`
}

export function num(value: number | null | undefined, digits = 0, suffix = ''): string {
  if (value === null || value === undefined || Number.isNaN(value)) return '—'
  return value.toFixed(digits) + suffix
}

export function bytes(value?: number | null): string {
  if (!value) return '—'
  if (value < 1024) return `${value} B`
  return `${(value / 1024).toFixed(1)} kB`
}

export function percent(ratio?: number | null, digits = 0): string {
  if (ratio === null || ratio === undefined) return '—'
  return `${(ratio * 100).toFixed(digits)}%`
}
