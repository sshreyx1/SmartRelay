import type { ConnectionState } from '../lib/types'

const LABEL: Record<ConnectionState, string> = {
  connecting: 'connecting',
  live: 'live',
  offline: 'reconnecting',
}

export function ConnectionBadge({ state }: { state: ConnectionState }) {
  return (
    <div className={`conn is-${state}`}>
      <span className="conn-dot" />
      <span>{LABEL[state]}</span>
    </div>
  )
}
