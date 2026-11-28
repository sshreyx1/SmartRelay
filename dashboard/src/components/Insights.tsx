import type { Insight } from '../lib/types'
import { Panel } from './ui'

export function Insights({ items }: { items: Insight[] | null }) {
  return (
    <Panel title="Automated key insights" right={<span className="muted-strong">from current filters</span>}>
      {items == null ? (
        <div className="skeleton" />
      ) : (
        <ol className="insights">
          {items.map((i, k) => (
            <li key={k} className={i.level === 'warn' ? 'is-warn' : ''}>
              <span className="insight-n">{i.level === 'warn' ? '!' : k + 1}</span>
              <div>
                <div className="insight-text">{i.text}</div>
                <div className="insight-ev">{i.evidence}</div>
              </div>
            </li>
          ))}
        </ol>
      )}
    </Panel>
  )
}
