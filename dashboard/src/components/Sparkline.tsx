interface Props {
  values: (number | null)[]
  /** Internal coordinate space for the path math — NOT the rendered size.
   *  The rendered size is set by CSS (width: 100%) so the chart actually
   *  scales with its container, including on window resize. */
  viewWidth?: number
  viewHeight?: number
  color: string
  fillColor?: string
  /** Rendered height in CSS pixels. Width is always 100% of the parent. */
  height?: number
}

/**
 * A minimal line chart drawn as plain SVG — no charting library, since one
 * series with maybe 200 points doesn't need one and it keeps the dashboard's
 * dependency list at just React.
 *
 * Sized with viewBox + preserveAspectRatio="none" and CSS width:100%, rather
 * than fixed SVG width/height attributes. A fixed-pixel SVG doesn't shrink
 * when its container does — on a resize down to a narrow window the chart
 * would keep its original width and get clipped or force an overflow. This
 * way the coordinate math still runs in a stable space (viewWidth x
 * viewHeight) but the actual on-screen size follows the container.
 *
 * Null points (a reading that failed on the node) are skipped rather than
 * plotted as zero, which would read as a real drop to zero on the chart.
 */
export function Sparkline({
  values,
  viewWidth = 220,
  viewHeight = 44,
  color,
  fillColor,
  height = 44,
}: Props) {
  const points = values
    .map((v, i) => (v == null ? null : { x: i, y: v }))
    .filter((p): p is { x: number; y: number } => p !== null)

  const svgProps = {
    viewBox: `0 0 ${viewWidth} ${viewHeight}`,
    preserveAspectRatio: 'none' as const,
    style: { width: '100%', height, display: 'block' as const },
    'aria-hidden': true as const,
  }

  if (points.length < 2) {
    return (
      <svg {...svgProps}>
        <line
          x1={0}
          y1={viewHeight - 1}
          x2={viewWidth}
          y2={viewHeight - 1}
          stroke={color}
          strokeOpacity={0.15}
          strokeWidth={1}
        />
      </svg>
    )
  }

  const xs = points.map((p) => p.x)
  const ys = points.map((p) => p.y)
  const minX = Math.min(...xs)
  const maxX = Math.max(...xs)
  const minY = Math.min(...ys)
  const maxY = Math.max(...ys)
  const spanY = maxY - minY || 1
  const spanX = maxX - minX || 1

  const pad = 3
  const scaleX = (x: number) =>
    pad + ((x - minX) / spanX) * (viewWidth - pad * 2)
  const scaleY = (y: number) =>
    viewHeight - pad - ((y - minY) / spanY) * (viewHeight - pad * 2)

  const path = points
    .map((p, i) => `${i === 0 ? 'M' : 'L'}${scaleX(p.x).toFixed(1)},${scaleY(p.y).toFixed(1)}`)
    .join(' ')

  const areaPath = fillColor
    ? `${path} L${scaleX(points[points.length - 1].x).toFixed(1)},${viewHeight} L${scaleX(points[0].x).toFixed(1)},${viewHeight} Z`
    : null

  const last = points[points.length - 1]

  return (
    <svg {...svgProps}>
      {areaPath && <path d={areaPath} fill={fillColor} stroke="none" />}
      <path d={path} fill="none" stroke={color} strokeWidth={1.5} strokeLinejoin="round" strokeLinecap="round" vectorEffect="non-scaling-stroke" />
      <circle cx={scaleX(last.x)} cy={scaleY(last.y)} r={2.2} fill={color} />
    </svg>
  )
}
