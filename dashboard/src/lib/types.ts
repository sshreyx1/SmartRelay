/**
 * Types mirroring the REAL backend contract (backend/app.py, storage.py).
 *
 * This dashboard is a UI port of the old trail-mesh demo app (same layout,
 * same colour scheme, same components) rebuilt against the actual ALFP
 * wire data instead of a simulated one. A few fields the old UI showed --
 * species identification, review workflow, per-fragment live progress,
 * RSSI/SNR, battery, duplicate/retransmit counts -- simply don't exist in
 * the current protocol or backend yet. Those are marked `TBD` below and the
 * components that use them render a plain "TBD" note instead of inventing a
 * number. Wiring one up later is: add the field to the real pipeline, fill
 * it in here, and the matching UI piece lights up on its own.
 */

export type NodeStatus = 'recent' | 'stale' | 'never seen'
export type NodeRole = 'bridge' | 'field'

export interface MeshNode {
  node_id: number
  role: NodeRole
  last_seen: number | null
  seconds_since: number | null
  last_radio: string | null
  status: NodeStatus
}

export interface Stats {
  real_captures: number
  demo_captures: number
  by_radio: Record<string, number>
}

export interface HealthInfo {
  ok: boolean
  bridge_port: string | null
  bridge_connected: boolean
  dashboards_connected: number
  stats: Stats
}

/**
 * One real, verified ALFP transfer reassembled by the bridge. This is both
 * "a sighting" (it has a photo + sensor readings) and "a completed
 * transfer" (it has frag_total/radio/crc32) in the old UI's vocabulary --
 * the real pipeline only ever gives us the finished, CRC-checked file, so
 * there is one shape here instead of two.
 *
 * "-" / null means the firmware's capture_meta.h sent its MISSING/UNKNOWN
 * sentinel for that field (see common/capture_meta.h) -- never a guess.
 */
export interface Capture {
  id: number
  node_id: number
  capture_id: number | null
  session_id: number
  radio: string | null
  frag_total: number | null
  size_bytes: number | null
  crc32: string | null
  image_path: string | null
  image_url: string | null
  received_at: number

  uptime_ms: number | null
  trigger_pir: 0 | 1
  trigger_sound: 0 | 1
  temperature_c: number | null
  humidity_pct: number | null
  light_raw: number | null
  weather_label: number | null
  weather_name: string | null
  trigger_trust: number | null
  sound_class: number | null
  sound_name: string | null
  sound_rms: number | null
  sound_zcr: number | null
  image_index: number | null

  is_demo: 0 | 1
}

export interface LogLine {
  id: number
  line: string
  received_at: number
}

export interface HourBucket {
  hour: number
  count: number
}

/** A capture's temp/humidity/light plotted over time for one node -- real,
 *  derived client-side from the captures already on hand (there is no
 *  separate telemetry/heartbeat table yet; every real reading we have
 *  comes attached to a capture). */
export interface EnvironmentPoint {
  received_at: number
  temperature_c: number | null
  humidity_pct: number | null
  light_raw: number | null
}

// --- WebSocket envelope ----------------------------------------------------

export interface SnapshotPayload {
  nodes: MeshNode[]
  captures: Capture[]
  log: LogLine[]
  stats: Stats
}

export type WsEvent =
  | { event: 'snapshot'; data: SnapshotPayload; ts: number }
  | { event: 'capture'; data: Capture; ts: number }
  | { event: 'log'; data: { line: string }; ts: number }

export type NotificationLevel = 'info' | 'warning' | 'error'

export interface AppNotification {
  id: number
  level: NotificationLevel
  title: string
  body: string
  at: Date
  read: boolean
}

export type ConnectionState = 'connecting' | 'live' | 'offline'
export type Theme = 'light' | 'dark'
