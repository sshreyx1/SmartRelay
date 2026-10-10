import { useCallback, useEffect, useState } from 'react'
import { CaptureFeed } from '../components/CaptureFeed'
import { api } from '../lib/api'
import type { LiveMesh } from '../hooks/useLiveMesh'
import type { Capture } from '../lib/types'

const PAGE_SIZE = 20

export function CapturesPage({ live }: { live: LiveMesh }) {
  const { stats } = live
  const [items, setItems] = useState<Capture[]>([])
  const [requested, setRequested] = useState(PAGE_SIZE)
  const [loading, setLoading] = useState(true)

  const total = (stats?.real_captures ?? 0) + (stats?.demo_captures ?? 0)

  const load = useCallback(async (limit: number) => {
    setLoading(true)
    try {
      setItems(await api.captures({ limit }))
      setRequested(limit)
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    void load(PAGE_SIZE)
  }, [load])

  return (
    <>
      <div className="page-head" style={{ marginBottom: 22 }}>
        <div>
          <h1>All captures</h1>
          <div className="page-sub">
            {total} capture{total === 1 ? '' : 's'} recorded ({stats?.demo_captures ?? 0} simulated)
          </div>
        </div>
      </div>

      <CaptureFeed
        captures={items}
        title=""
        showCount={false}
        footer={
          items.length < total ? (
            <div className="feed-footer">
              <button className="btn btn-ghost" onClick={() => void load(requested + PAGE_SIZE)} disabled={loading}>
                {loading ? 'Loading…' : `Load more (${total - items.length} remaining)`}
              </button>
            </div>
          ) : undefined
        }
      />
    </>
  )
}
