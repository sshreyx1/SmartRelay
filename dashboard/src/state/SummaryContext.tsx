import { createContext, useContext, type ReactNode } from 'react'
import { api } from '../lib/api'
import { useFilters } from '../lib/filters'
import { usePolling, type Polled } from '../hooks/usePolling'
import type { Summary } from '../lib/types'

/** /api/summary for the current filters, polled once for the whole app (header range, node tabs, overview). */
const Ctx = createContext<Polled<Summary> | null>(null)

export function SummaryProvider({ children }: { children: ReactNode }) {
  const { filters, key } = useFilters()
  const polled = usePolling((s) => api.summary(filters, s), key)
  return <Ctx.Provider value={polled}>{children}</Ctx.Provider>
}

export function useSummary(): Polled<Summary> {
  const v = useContext(Ctx)
  if (!v) throw new Error('useSummary outside SummaryProvider')
  return v
}
