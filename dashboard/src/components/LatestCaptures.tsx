import { useNavigate } from 'react-router-dom'
import { useHref } from '../lib/filters'
import { clock, imageFile, soundName } from '../lib/format'
import type { Capture } from '../lib/types'
import { Note, Panel, RadioTag, SourceTag } from './ui'

export function LatestCaptures({ captures }: { captures: Capture[] | null }) {
  const navigate = useNavigate()
  const href = useHref()
  return (
    <Panel title="Latest captures" right={<span className="muted-strong">newest 20 · click for details</span>} className="latest">
      {captures == null ? (
        <div className="skeleton" />
      ) : captures.length === 0 ? (
        <div className="empty-line">No captures match these filters.</div>
      ) : (
        <ul className="latest-list">
          {captures.map((c) => {
            const img = imageFile(c)
            return (
              <li key={c.id}>
                <button onClick={() => navigate(href(`/capture/${c.id}`))}>
                  <span className="mono">{clock(c.received_at)}</span>
                  <span className="latest-what">
                    <strong>Node {c.node_id}</strong> ·{' '}
                    {c.sound_label ? (
                      <>
                        <strong>{soundName(c.sound_label)}</strong>
                        {c.sound_conf != null && ` ${c.sound_conf}%`}
                      </>
                    ) : (
                      'sound class unavailable'
                    )}
                  </span>
                  <RadioTag radio={img?.radio ?? c.radio} />
                  <SourceTag demo={c.is_demo} />
                </button>
              </li>
            )
          })}
        </ul>
      )}
      <Note>Sound classes come from the node's on-board classifier. They are not confirmed species.</Note>
    </Panel>
  )
}
