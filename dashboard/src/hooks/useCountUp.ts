import { useEffect, useRef, useState } from 'react'

/** Animates a number to its new value over 400 ms. Jumps straight there when motion is reduced. */
export function useCountUp(value: number | null, reduced: boolean, ms = 400): number | null {
  const [shown, setShown] = useState(value)
  const from = useRef(value)
  useEffect(() => {
    if (value == null || reduced || from.current == null || from.current === value) {
      from.current = value
      setShown(value)
      return
    }
    const start = performance.now()
    const a = from.current
    let raf = 0
    const step = (t: number) => {
      const k = Math.min(1, (t - start) / ms)
      const eased = 1 - Math.pow(1 - k, 3)
      setShown(Math.round(a + (value - a) * eased))
      if (k < 1) raf = requestAnimationFrame(step)
      else from.current = value
    }
    raf = requestAnimationFrame(step)
    return () => {
      cancelAnimationFrame(raf)
      from.current = value
    }
  }, [value, reduced, ms])
  return shown
}
