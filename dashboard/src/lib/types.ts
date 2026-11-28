/**
 * Types mirroring the backend contract (backend/app.py, storage.py) field for field.
 *
 * null always means "the source cannot answer" and the UI shows "Data unavailable" for it.
 * A 0 is only ever shown when the backend confirms zero.
 */

export type NodeStatus = 'recent' | 'stale' | 'never seen'
export type NodeRole = 'bridge' | 'field'
export type Mode = 'real' | 'sim' | 'both'
export type RadioFilter = 'lora' | 'wifi' | 'unknown'
export type FileKind = 'image' | 'telemetry' | 'test' | null

export interface MeshNode {
  node_id: number
  role: NodeRole
  /** time of the last REAL capture received from this node (not a heartbeat) */
  last_seen: number | null
  seconds_since: number | null
  last_radio: string | null
  status: NodeStatus
  /** a file from this node is being uploaded by the bridge right now */
  arriving?: boolean
}

export interface Stats {
  real_captures: number
  demo_captures: number
  by_radio: Record<string, number>
}

export interface BridgeEvent {
  connected: boolean
  at: number
}

export interface BridgeState {
  connected: boolean
  port: string | null
  since: number | null
  history: BridgeEvent[]
}

export interface HealthInfo {
  ok: boolean
  server_time: number
  bridge_port: string | null
  bridge_connected: boolean
  bridge: BridgeState
  dashboards_connected: number
  stats: Stats
}

/** One fragment as the bridge reported it (@EVT RXF). via = node that delivered it (null = unknown, LoRa). */
export interface Fragment {
  seq: number
  round: number | null
  hops: number | null
  via: number | null
  t_rel_s: number | null
  dup_count: number
}

/**
 * One file (ALFP transfer) as the bridge saw it: backend/transfers.py TransferTracker.transfer_view().
 * A capture usually sends two: telemetry + image. *_ms = bridge clock, *_at = laptop clock.
 * null = not reported (old bridge firmware without @EVT lines, or the backend started mid-transfer).
 */
export interface Transfer {
  id: number
  node_id: number
  sid: number
  capture_row_id: number | null
  capture_id: number | null
  kind: FileKind
  radio: string | null
  size_bytes: number | null
  frag_count: number | null
  frag_size: number | null
  crc_sender: string | null
  crc_bridge_ok: 0 | 1 | null
  crc_laptop_ok: 0 | 1 | null
  status: 'in_progress' | 'verified' | 'failed'
  fail_reason: string | null
  first_ms: number | null
  done_ms: number | null
  first_at: number | null
  verified_at: number | null
  upload_start_at: number | null
  upload_end_at: number | null
  /** ms the file waited in the sender's queue (node clock, capture meta v2) */
  queued_ms: number | null
  nacks: number | null
  dups: number | null
  rounds: number | null
  hops_min: number | null
  hops_max: number | null
  last_at: number | null
  is_demo: boolean | 0 | 1
  /** distinct fragments received so far */
  have: number
  /** fragments per delivering node id ("0" = unknown) */
  via_counts: Record<string, number>
  radio_s: number | null
  upload_s: number | null
  fragments?: Fragment[]
}

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
  sound_conf?: number | null
  sound_label?: string | null
  sound_group?: string | null
  image_name?: string | null
  display_id?: string | null
  is_demo: 0 | 1
  files: Transfer[]
}

export interface SoundClassRow {
  sound_class: number
  sound_label: string
  sound_group: string | null
  count: number
  mean_conf: number | null
  nodes: number[]
  last_seen: number
  sim_count: number
}

export interface RadioMixRow {
  radio: string
  node_id: number
  kind: string
  files: number
  avg_size: number | null
  avg_frags: number | null
  max_size: number | null
  duplicates: number | null
}

export interface Insight {
  level: 'info' | 'warn'
  text: string
  evidence: string
}

export interface TimelineBin {
  t: number
  real: number
  sim: number
}

export interface Summary {
  range: { first: number | null; last: number | null }
  filtered_range: { first: number | null; last: number | null }
  captures: number
  real_captures: number
  sim_captures: number
  dual_sensor: { count: number; of: number }
  crc_verified_transfers: number
  failed_transfers: number
  laptop_crc_failed: number
  field_nodes_reporting: { count: number; of: number; window_s: number; basis: string; nodes: number[] } | null
  by_node: Record<string, number>
  by_radio: { lora: number; wifi: number; unknown: number }
  timeline: { bin_s: number; bins: TimelineBin[] }
  /** bridge-measured radio time of image files, per radio; null = never reported */
  median_transfer_s: { wifi: number | null; lora: number | null } | null
  median_usb_upload_s: number | null
  sound_classes: SoundClassRow[]
  radio_mix: { rows: RadioMixRow[]; note: string }
  insights: Insight[]
}

export interface ReadingPoint {
  id: number
  received_at: number
  temperature_c: number | null
  humidity_pct: number | null
  light_raw: number | null
  sound_rms: number | null
  is_demo: 0 | 1
}

export interface FieldNodeDetail {
  node_id: number
  role: 'field'
  captures_count: number
  last_capture_at: number | null
  last_radio: { radio: string | null; kind: FileKind; received_at: number } | null
  median_image_transfer_s: number | null
  median_image_transfer_radio: string
  fragments_resent: { resent: number; of: number } | null
  files: {
    files: number
    frags: number | null
    dup: number | null
    dup_known: number | null
    wifi_files: number | null
    lora_files: number | null
  }
  captures: Capture[]
  readings: ReadingPoint[]
  /** Wi-Fi fragments per delivering node ("direct" = straight from the sender, "unknown"); LoRa total */
  relay_counts: { wifi: Record<string, number>; lora: number } | null
}

export interface LogLine {
  id: number
  line: string
  received_at: number
}

export interface BridgeDetail {
  node_id: 1
  role: 'bridge'
  bridge: BridgeState
  transfers: {
    verified: number | null
    failed: number | null
    laptop_failed: number | null
    laptop_ok: number | null
    real: number | null
    fail_reasons: Record<string, number>
    bridge_crc_retries: number
  }
  in_flight: Transfer[]
  log: LogLine[]
}

// --- WebSocket envelope ----------------------------------------------------

export interface SnapshotPayload {
  nodes: MeshNode[]
  captures: Capture[]
  log: LogLine[]
  stats: Stats
  in_flight: Transfer[]
  bridge: BridgeState
}

export interface StartPayload {
  transfer_id: number
  node_id: number
  sid: number
  radio: string | null
  first_at: number | null
  is_demo: boolean
}

export interface ProgressPayload {
  transfer_id: number
  node_id: number
  sid: number
  radio: string | null
  have: number
  frag_count: number | null
  kind: FileKind
  capture_id: number | null
  last_hops: number | null
  last_via: number | null
  via_counts: Record<string, number>
  nacks: number | null
  dups: number | null
  elapsed_s: number | null
  is_demo: boolean
}

export interface FailedPayload {
  transfer_id: number
  reason: string
  node_id: number
  sid: number
  is_demo: boolean
}

export type WsEvent =
  | { event: 'snapshot'; data: SnapshotPayload; ts: number }
  | { event: 'capture'; data: Capture; ts: number }
  | { event: 'log'; data: { line: string }; ts: number }
  | { event: 'transfer_start'; data: StartPayload; ts: number }
  | { event: 'transfer_progress'; data: ProgressPayload; ts: number }
  | { event: 'transfer_done'; data: Transfer; ts: number }
  | { event: 'transfer_failed'; data: FailedPayload; ts: number }
  | { event: 'bridge_status'; data: BridgeState; ts: number }

export type NotificationLevel = 'info' | 'warning' | 'error'

export interface AppNotification {
  id: number
  level: NotificationLevel
  title: string
  body: string
  at: Date
  read: boolean
  tag?: 'REAL' | 'SIMULATED'
}

export type ConnectionState = 'connecting' | 'live' | 'offline'

/** GET /api/tests/suites and /api/tests/runs/{id} (backend/suite_runner.py). */
export type RunStatus = 'running' | 'passed' | 'failed' | 'cancelled' | 'error'

export interface TestCheck {
  name: string
  passed: boolean
  detail: string
}

export interface TestRun {
  id: number
  suite: string
  status: RunStatus
  step: string
  started_at: number
  finished_at: number | null
  exit_code: number | null
  summary: string
  checks: TestCheck[]
  line_count: number
  lines?: string[]
}

export interface TestSuite {
  id: string
  name: string
  proves: string
  duration: string
  commands: string[]
  available: boolean
  missing: string[]
  last: TestRun | null
}
