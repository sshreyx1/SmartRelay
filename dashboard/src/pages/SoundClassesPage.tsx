import { SoundClassBars } from '../components/SoundClassBars'
import { Panel } from '../components/ui'
import { useSummary } from '../state/SummaryContext'

export function SoundClassesPage() {
  const { data: s, error } = useSummary()
  return (
    <div className="page">
      <div className="page-head">
        <div>
          <h1>Sound classes reported by nodes (not confirmed species)</h1>
          <p className="page-sub">
            Each node's on-board classifier labels the sound at capture time (group, then class). The backend only
            counts the labels. Filters apply.
          </p>
        </div>
      </div>
      {error && !s && <div className="error-line">Could not load: {error}</div>}
      <Panel title="Count, average confidence and reporting nodes per class">
        {s ? <SoundClassBars rows={s.sound_classes} /> : <div className="skeleton" />}
      </Panel>
    </div>
  )
}
