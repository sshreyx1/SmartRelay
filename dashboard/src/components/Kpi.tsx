import type { ReactNode } from 'react'
import { useCountUp } from '../hooks/useCountUp'
import { useLive } from '../state/LiveContext'

export function Kpi({
  label,
  value,
  of,
  sub,
  hero,
  side,
  unavailable,
  tag,
}: {
  label: string
  value: number | null
  of?: number | null
  sub: string
  hero?: boolean
  side?: ReactNode
  /** shown instead of the number when the source cannot answer */
  unavailable?: ReactNode
  tag?: ReactNode
}) {
  const { reducedMotion } = useLive()
  const shown = useCountUp(value, reducedMotion)
  return (
    <div className={'kpi' + (hero ? ' is-hero' : '')}>
      <div className="kpi-main">
        <div className="kpi-label">{label}</div>
        {unavailable ?? (
          <div className="kpi-value">
            {shown ?? '—'}
            {of != null && <span className="kpi-of"> of {of}</span>}
            {tag}
          </div>
        )}
        <div className="kpi-sub">{sub}</div>
      </div>
      {side && <div className="kpi-side">{side}</div>}
    </div>
  )
}
