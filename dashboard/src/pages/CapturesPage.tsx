import { CaptureTable } from '../components/CaptureTable'
import { Panel } from '../components/ui'

export function CapturesPage() {
  return (
    <div className="page">
      <div className="page-head">
        <div>
          <h1>Capture records</h1>
          <p className="page-sub">Newest first. Click a row to see its files, CRC check and fragmentation.</p>
        </div>
      </div>
      <Panel>
        <CaptureTable />
      </Panel>
    </div>
  )
}
