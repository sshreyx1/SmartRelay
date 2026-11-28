import type { ReactNode } from 'react'
import { radioName, TRIGGER_LABEL, triggerOf, UNAVAILABLE } from '../lib/format'
import type { Capture } from '../lib/types'

export function RadioTag({ radio }: { radio: string | null | undefined }) {
  const cls = radio === 'wifi' ? 'is-wifi' : radio === 'lora' ? 'is-lora' : 'is-unknown'
  return <span className={`tag ${cls}`}>{radioName(radio)}</span>
}

export function SourceTag({ demo }: { demo: 0 | 1 | boolean }) {
  return demo ? <span className="tag is-sim">SIMULATED</span> : <span className="tag is-real">REAL</span>
}

export function TriggerChip({ capture }: { capture: Pick<Capture, 'trigger_pir' | 'trigger_sound'> }) {
  const t = triggerOf(capture)
  return (
    <span className={`tag is-trigger t-${t}`} title="Which sensors fired when the capture was taken">
      {TRIGGER_LABEL[t]}
    </span>
  )
}

/** "Data unavailable", with the reason on hover and optionally below. */
export function Unavailable({ why, inline }: { why?: string; inline?: boolean }) {
  return (
    <span className={inline ? 'unavail is-inline' : 'unavail'} title={why}>
      {UNAVAILABLE}
      {why && !inline && <small>{why}</small>}
    </span>
  )
}

export function Panel({
  title,
  right,
  children,
  className,
}: {
  title?: ReactNode
  right?: ReactNode
  children: ReactNode
  className?: string
}) {
  return (
    <section className={'panel' + (className ? ` ${className}` : '')}>
      {(title || right) && (
        <header className="panel-head">
          {title && <h2>{title}</h2>}
          {right && <div className="panel-right">{right}</div>}
        </header>
      )}
      {children}
    </section>
  )
}

export function Note({ children }: { children: ReactNode }) {
  return <div className="note">{children}</div>
}

export function Segmented<T extends string>({
  value,
  options,
  onChange,
  label,
}: {
  value: T
  options: { value: T; label: string }[]
  onChange: (v: T) => void
  label: string
}) {
  return (
    <div className="seg" role="radiogroup" aria-label={label}>
      {options.map((o) => (
        <button
          key={o.value}
          role="radio"
          aria-checked={o.value === value}
          className={o.value === value ? 'is-on' : ''}
          onClick={() => onChange(o.value)}
        >
          {o.label}
        </button>
      ))}
    </div>
  )
}
