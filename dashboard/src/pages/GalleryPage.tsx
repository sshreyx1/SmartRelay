import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api } from '../lib/api'
import { useFilters, useHref } from '../lib/filters'
import { clock, imageFile, routeLabel, secs, size, soundName, uploadSeconds } from '../lib/format'
import { usePolling } from '../hooks/usePolling'
import { RadioTag, Segmented, SourceTag } from '../components/ui'
import type { Capture } from '../lib/types'

/** Proof that images arrive whole: the newest 20 images, each accepted only after its CRC-32 matched. */
export function GalleryPage() {
  const { filters, key } = useFilters()
  const [layout, setLayout] = useState<'grid' | 'large'>('grid')
  const { data, error } = usePolling((s) => api.images(filters, 20, s), key)

  return (
    <div className="page">
      <div className="page-head">
        <div>
          <h1>Latest 20 images received</h1>
          <p className="page-sub">
            Each image was cut into fragments by its field node, sent over the mesh, rebuilt by Node 1, and accepted
            only when its CRC-32 matched. It is shown here exactly as received.
          </p>
        </div>
        <Segmented
          label="Layout"
          value={layout}
          onChange={setLayout}
          options={[
            { value: 'grid', label: 'Grid' },
            { value: 'large', label: 'Large' },
          ]}
        />
      </div>
      {error && <div className="error-line">Could not load images: {error}</div>}
      {data && data.captures.length === 0 && <div className="empty-line">No images match these filters.</div>}
      <div className={layout === 'large' ? 'gallery is-large' : 'gallery'}>
        {data?.captures.map((c) => <ImageCard key={c.id} c={c} />)}
      </div>
      {data && data.captures.length > 0 && (
        <p className="foot-note">
          Showing {data.captures.length} of {data.total} images · click an image to see how it was fragmented and sent
        </p>
      )}
    </div>
  )
}

function ImageCard({ c }: { c: Capture }) {
  const navigate = useNavigate()
  const href = useHref()
  const img = imageFile(c)
  const up = uploadSeconds(img)
  const route = routeLabel(img)
  const crcOk = img?.crc_laptop_ok === 1
  return (
    <article
      className={'card' + (c.is_demo ? ' is-sim' : '')}
      onClick={() => navigate(href(`/capture/${c.id}`))}
      tabIndex={0}
      onKeyDown={(e) => e.key === 'Enter' && navigate(href(`/capture/${c.id}`))}
    >
      <div className="card-img">
        {c.image_url ? <img src={c.image_url} alt={`Node ${c.node_id} capture`} loading="lazy" /> : null}
        <span className="card-badge left">
          <SourceTag demo={c.is_demo} />
        </span>
        <span className={'card-badge right crc' + (crcOk ? '' : ' is-muted')}>{crcOk ? '✓ CRC match' : 'CRC not rechecked'}</span>
        {c.image_url && (
          <a
            className="card-full"
            href={c.image_url}
            target="_blank"
            rel="noreferrer"
            onClick={(e) => e.stopPropagation()}
            aria-label="Open full-size image"
          >
            ⤢
          </a>
        )}
      </div>
      <div className="card-body">
        <div className="card-row">
          <strong>Node {c.node_id}</strong>
          <span className="mono muted">{clock(c.received_at)}</span>
        </div>
        <div className="muted">
          sound class <strong className="ink">{soundName(c.sound_label)}</strong>
          {c.sound_conf != null && ` ${c.sound_conf}%`}
        </div>
        <div className="card-row is-start">
          <RadioTag radio={img?.radio ?? c.radio} />
          <span>
            {img?.frag_count ?? c.frag_total ?? '—'} frags
            {route ? ` · ${route}` : ` · ${size(img?.size_bytes ?? c.size_bytes)}`}
          </span>
        </div>
        <div className="card-row is-start">
          {img?.radio_s != null ? (
            <span>
              sent in <strong>{secs(img.radio_s)}</strong>
            </span>
          ) : up != null ? (
            <span>
              USB upload <strong>{secs(up)}</strong>
            </span>
          ) : (
            <span className="muted">timing unavailable</span>
          )}
          {img?.nacks != null && img.nacks > 0 ? (
            <span className="amber">
              {img.nacks} NACK{img.nacks === 1 ? '' : 's'} · {img.rounds ?? '?'} rounds
            </span>
          ) : (
            img?.dups != null &&
            img.dups > 0 && <span className="muted">{img.dups} duplicate{img.dups === 1 ? '' : 's'} ignored</span>
          )}
        </div>
      </div>
    </article>
  )
}
