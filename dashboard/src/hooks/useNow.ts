import { useSyncExternalStore } from 'react'

/** One shared 1-second clock so every "33 s ago" on the page re-renders together. */
let now = Date.now() / 1000
const listeners = new Set<() => void>()
let timer: number | null = null

function subscribe(cb: () => void) {
  listeners.add(cb)
  if (timer == null) {
    timer = window.setInterval(() => {
      now = Date.now() / 1000
      listeners.forEach((l) => l())
    }, 1000)
  }
  return () => {
    listeners.delete(cb)
    if (listeners.size === 0 && timer != null) {
      window.clearInterval(timer)
      timer = null
    }
  }
}

export function useNow(): number {
  return useSyncExternalStore(
    subscribe,
    () => now,
    () => now,
  )
}
