import { useEffect, useState } from 'react'
import { api } from '../lib/api'
import type { Capture, SpeciesTally } from '../lib/types'

/**
 * What the NODE heard. The label, its group and the confidence are decided on the Pico by the
 * two-stage sound classifier (firmware/sensing/sound_classify.c) and only displayed here -- the
 * backend never classifies (SOUND_PLAN.md rule 9).
 */

const GROUPS = new Set(['AMBIENT', 'BIRD', 'MAMMAL', 'INSECT_FROG', 'HUMAN', 'VEHICLE', 'WEATHER'])
const PRETTY: Record<string, string> = { INSECT_FROG: 'Insect / frog', INSECT_BUZZ: 'Insect buzz' }

/** "DOG" -> "Dog", "INSECT_FROG" -> "Insect / frog" */
export function prettyLabel(label: string | null | undefined): string {
  if (!label) return 'Unidentified'
  return PRETTY[label] ?? label.charAt(0) + label.slice(1).toLowerCase().replace(/_/g, ' ')
}

export function isGroupOnly(label: string | null | undefined): boolean {
  return !!label && GROUPS.has(label)
}

/** "N2-000042": the node's own name for the capture (common/capture_id.h). */
export function displayId(c: Pick<Capture, 'display_id' | 'node_id' | 'capture_id'>): string {
  if (c.display_id) return c.display_id
  return c.capture_id != null ? `N${c.node_id}-${String(c.capture_id).padStart(6, '0')}` : `N${c.node_id}-?`
}

export function IdBadge({ capture, large }: { capture: Capture; large?: boolean }) {
  return (
    <span
      className={'id-badge' + (large ? ' is-large' : '')}
      title="Capture id: the same name the node logs and uses on its SD card"
    >
      {displayId(capture)}
    </span>
  )
}

/** Title + subtitle for a capture card: "Dog" / "identified on-node", or "Bird (unidentified)". */
export function SoundHeading({ capture, size }: { capture: Capture; size?: number }) {
  const label = capture.sound_label
  const style = size ? { fontSize: size } : undefined
  if (!label) {
    return (
      <div>
        <div className="sighting-name is-unknown" style={style}>
          Unidentified
        </div>
        <div className="sighting-sci">
          {capture.sound_class == null ? 'no sound label from the node' : 'label not in this dashboard\'s table'}
        </div>
      </div>
    )
  }
  return (
    <div>
      <div className={'sighting-name' + (isGroupOnly(label) ? ' is-unknown' : '')} style={style}>
        {prettyLabel(label)}
        {isGroupOnly(label) && label !== 'AMBIENT' ? ' (unidentified)' : ''}
      </div>
      <div className="sighting-sci">identified on-node</div>
    </div>
  )
}

/** The label chip, e.g. "DOG · 82% · MAMMAL" or "BIRD (unidentified)". */
export function SoundChip({ capture }: { capture: Capture }) {
  const label = capture.sound_label
  if (!label) return null
  const conf = capture.sound_conf != null ? ` · ${capture.sound_conf}%` : ''
  const text = isGroupOnly(label)
    ? `${label}${label === 'AMBIENT' ? '' : ' (unidentified)'}${conf}`
    : `${label}${conf}${capture.sound_group ? ` · ${capture.sound_group}` : ''}`
  return (
    <span className="chip is-sound" title="Decided on the node by its two-stage sound classifier">
      {text}
    </span>
  )
}

/** /api/species, refetched whenever the number of captures changes. */
export function useSpeciesTally(refreshKey: number, includeDemo: boolean) {
  const [rows, setRows] = useState<SpeciesTally[] | null>(null)
  const [error, setError] = useState<string | null>(null)
  useEffect(() => {
    let cancelled = false
    api
      .species(includeDemo)
      .then((r) => {
        if (!cancelled) {
          setRows(r)
          setError(null)
        }
      })
      .catch((e: Error) => !cancelled && setError(e.message))
    return () => {
      cancelled = true
    }
  }, [refreshKey, includeDemo])
  return { rows, error }
}
