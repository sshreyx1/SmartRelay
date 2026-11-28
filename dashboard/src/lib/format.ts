import type { Capture, Transfer } from './types'

export const UNAVAILABLE = 'Data unavailable'

/** "33 s ago" / "14 min ago" / "3 h ago". `now` comes from useNow() so it re-renders every second. */
export function ago(ts: number | null | undefined, now: number): string {
  if (ts == null) return UNAVAILABLE
  const d = Math.max(0, Math.floor(now - ts))
  if (d < 60) return `${d} s ago`
  if (d < 3600) return `${Math.floor(d / 60)} min ago`
  if (d < 86400) return `${Math.floor(d / 3600)} h ago`
  return `${Math.floor(d / 86400)} d ago`
}

export function clock(ts: number | null | undefined, seconds = true): string {
  if (ts == null) return '—'
  return new Date(ts * 1000).toLocaleTimeString('en-GB', {
    hour: '2-digit',
    minute: '2-digit',
    ...(seconds ? { second: '2-digit' } : {}),
    hour12: false,
  })
}

export function dayLabel(ts: number | null | undefined): string {
  if (ts == null) return '—'
  return new Date(ts * 1000).toLocaleDateString('en-GB', { day: 'numeric', month: 'short' })
}

/** "26 Nov 09:12 – 12:27", with both dates when the range crosses midnight. */
export function rangeLabel(a: number | null | undefined, b: number | null | undefined): string {
  if (a == null || b == null) return 'no data yet'
  const sameDay = new Date(a * 1000).toDateString() === new Date(b * 1000).toDateString()
  return sameDay
    ? `${dayLabel(a)} ${clock(a, false)} – ${clock(b, false)}`
    : `${dayLabel(a)} ${clock(a, false)} – ${dayLabel(b)} ${clock(b, false)}`
}

export function size(n: number | null | undefined): string {
  if (n == null) return '—'
  if (n < 1024) return `${Math.round(n)} B`
  return `${(n / 1024).toFixed(1)} KB`
}

export function secs(s: number | null | undefined): string {
  if (s == null) return '—'
  return s < 10 ? `${s.toFixed(1)} s` : `${Math.round(s)} s`
}

export function radioName(r: string | null | undefined): string {
  if (r === 'wifi') return 'Wi-Fi'
  if (r === 'lora') return 'LoRa'
  return 'Unknown'
}

/** "DOG" -> "dog", "INSECT_BUZZ" -> "insect buzz" (sound class, not a species) */
export function soundName(label: string | null | undefined): string {
  if (!label) return 'unavailable'
  return label.toLowerCase().replace(/_/g, ' ')
}

export function captureName(c: Pick<Capture, 'display_id' | 'node_id' | 'capture_id'>): string {
  if (c.display_id) return c.display_id
  return c.capture_id != null ? `N${c.node_id}-${String(c.capture_id).padStart(6, '0')}` : `N${c.node_id}-?`
}

export type TriggerKind = 'dual' | 'pir' | 'sound' | 'manual'

export function triggerOf(c: Pick<Capture, 'trigger_pir' | 'trigger_sound'>): TriggerKind {
  if (c.trigger_pir && c.trigger_sound) return 'dual'
  if (c.trigger_pir) return 'pir'
  if (c.trigger_sound) return 'sound'
  return 'manual'
}

export const TRIGGER_LABEL: Record<TriggerKind, string> = {
  dual: 'PIR + sound',
  pir: 'PIR only',
  sound: 'sound only',
  manual: 'manual',
}

export function imageFile(c: Capture): Transfer | undefined {
  return c.files.find((f) => f.kind === 'image')
}

export function telemetryFile(c: Capture): Transfer | undefined {
  return c.files.find((f) => f.kind === 'telemetry')
}

/** USB upload time for one file, when both ends were seen. */
export function uploadSeconds(t: Transfer | undefined): number | null {
  return t?.upload_s ?? null
}

/** Bridge-measured radio time (first fragment -> verified), from @EVT RXOK. */
export function radioSeconds(t: Transfer | undefined): number | null {
  return t?.radio_s ?? null
}

/** Time the verified file waited on the bridge before its USB upload began (laptop clock both ends). */
export function bridgeQueueSeconds(t: Transfer | undefined): number | null {
  if (!t || t.verified_at == null || t.upload_start_at == null) return null
  return Math.max(0, t.upload_start_at - t.verified_at)
}

/** Relays that delivered fragments, busiest first: [[nodeId, count]]. Unknown (0) is left out. */
export function relays(t: Transfer | undefined): [number, number][] {
  if (!t) return []
  return Object.entries(t.via_counts ?? {})
    .map(([k, v]) => [Number(k), v] as [number, number])
    .filter(([k]) => k > 0)
    .sort((a, b) => b[1] - a[1])
}

/** "2 hops via C" / "1 hop direct" / "hops 2, relay unknown" / null when nothing was reported. */
export function routeLabel(t: Transfer | undefined): string | null {
  if (!t) return null
  const h = t.hops_max
  const hops = h != null ? `${h} hop${h === 1 ? '' : 's'}` : null
  const r = relays(t).filter(([n]) => n !== t.node_id)
  if (t.radio === 'lora') return hops ? `${hops}, relay unknown` : null
  if (h === 1) return `${hops} direct`
  if (r.length) return `${hops ? `${hops} ` : ''}via ${r.map(([n]) => LETTER[n]).join('+')}`
  return hops
}

/** Node letter from wifi_mesh_config.h (1=A bridge, 2=B ... 5=E). */
export const LETTER: Record<number, string> = { 1: 'A', 2: 'B', 3: 'C', 4: 'D', 5: 'E' }

/** Wi-Fi radio neighbours from third_party/wifi_mesh/wifi_mesh_config.h WIFI_MESH_NEIGHBOURS. */
export const NEIGHBOURS: Record<number, number[]> = {
  1: [2, 3, 4],
  2: [1, 3, 4],
  3: [1, 2, 4, 5],
  4: [1, 2, 3, 5],
  5: [3, 4],
}

export const FIELD_NODES = [2, 3, 4, 5]
