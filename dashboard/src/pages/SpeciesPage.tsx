import { useState } from 'react'
import { prettyLabel, isGroupOnly, useSpeciesTally } from '../components/SoundLabel'
import { timeAgo } from '../lib/api'
import type { LiveMesh } from '../hooks/useLiveMesh'
import type { SpeciesTally } from '../lib/types'

const GROUP_ORDER = ['BIRD', 'MAMMAL', 'INSECT_FROG', 'HUMAN', 'VEHICLE', 'WEATHER', 'AMBIENT']

/**
 * What the nodes heard, grouped by sound group: a tally of the labels the nodes' own on-device
 * classifier reported (/api/species). The backend only counts; the labels were decided on the
 * Pico. Phase A labels are ESC-50 animal sound types (dog, rooster, frog ...); Singapore species
 * arrive in Phase B with the same page.
 */
export function SpeciesPage({ live }: { live: LiveMesh }) {
  const [includeDemo, setIncludeDemo] = useState(false)
  const { rows, error } = useSpeciesTally(live.captures.length, includeDemo)

  const groups = new Map<string, SpeciesTally[]>()
  for (const r of rows ?? []) {
    const g = r.sound_group ?? 'UNKNOWN'
    groups.set(g, [...(groups.get(g) ?? []), r])
  }
  const ordered = [...groups.keys()].sort((a, b) => GROUP_ORDER.indexOf(a) - GROUP_ORDER.indexOf(b))

  return (
    <>
      <div className="page-head" style={{ marginBottom: 22 }}>
        <div>
          <h1>Species</h1>
          <div className="page-sub">What the nodes heard · identified on-node, counted here</div>
        </div>
        <label style={{ fontSize: 12.5, color: 'var(--text-3)', display: 'flex', gap: 6, alignItems: 'center' }}>
          <input type="checkbox" checked={includeDemo} onChange={(e) => setIncludeDemo(e.target.checked)} />
          include simulated captures
        </label>
      </div>

      {error && <div className="empty">Could not load /api/species: {error}</div>}
      {rows && rows.length === 0 && (
        <div className="empty">
          <strong>No labelled captures yet</strong>
          Each node classifies the sound of every capture itself (group, then animal) and sends the label with the
          image. Labelled captures will be counted here.
        </div>
      )}

      {ordered.map((g) => (
        <section key={g}>
          <div className="species-group-head">{prettyLabel(g)}</div>
          <div className="species-grid">
            {groups.get(g)!.map((r) => (
              <div className="species-card" key={r.sound_class}>
                <div className="species-card-count">{r.count}</div>
                <div className="species-card-name">
                  {prettyLabel(r.sound_label)}
                  {isGroupOnly(r.sound_label) && r.sound_label !== 'AMBIENT' ? ' (unidentified)' : ''}
                </div>
                <div className="species-card-meta">
                  last {timeAgo(r.last_seen)} · node{r.nodes.length === 1 ? '' : 's'} {r.nodes.join(', ')}
                </div>
                <div className="species-card-meta">
                  mean conf {r.mean_conf != null ? `${Math.round(r.mean_conf)}%` : '—'}
                  {r.demo_count > 0 ? ` · ${r.demo_count} simulated` : ''}
                </div>
              </div>
            ))}
          </div>
        </section>
      ))}
    </>
  )
}
