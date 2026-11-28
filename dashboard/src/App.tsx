import { useCallback, useState } from 'react'
import { BrowserRouter, Link, Route, Routes, useLocation } from 'react-router-dom'
import { FilterBar } from './components/FilterBar'
import { NodeTabs } from './components/NodeTabs'
import { Presenter } from './components/Presenter'
import { TopBar } from './components/TopBar'
import { useFilters } from './lib/filters'
import { BridgeLogPage } from './pages/BridgeLogPage'
import { CaptureDetailPage } from './pages/CaptureDetailPage'
import { CapturesPage } from './pages/CapturesPage'
import { GalleryPage } from './pages/GalleryPage'
import { NodesIndex, NodesPage } from './pages/NodesPage'
import { OverviewPage } from './pages/OverviewPage'
import { SoundClassesPage } from './pages/SoundClassesPage'
import { TestsPage } from './pages/TestsPage'
import { LiveProvider } from './state/LiveContext'
import { SummaryProvider, useSummary } from './state/SummaryContext'

export default function App() {
  return (
    <BrowserRouter>
      <LiveProvider>
        <SummaryProvider>
          <Shell />
        </SummaryProvider>
      </LiveProvider>
    </BrowserRouter>
  )
}

function Shell() {
  const [presenter, setPresenter] = useState(false)
  const closePresenter = useCallback(() => setPresenter(false), [])
  const { pathname } = useLocation()
  const { filters } = useFilters()
  const { data: s } = useSummary()
  const isTests = pathname.startsWith('/tests')
  const showFilters =
    !pathname.startsWith('/capture/') && !pathname.startsWith('/log') && pathname !== '/nodes/1' && !isTests

  return (
    <div className="app">
      <TopBar onPresenter={() => setPresenter(true)} />
      {showFilters && <FilterBar range={s?.range ?? null} />}
      {!isTests && <NodeTabs total={filters.node == null ? s?.captures ?? null : null} />}
      <main className="main">
        <Routes>
          <Route path="/" element={<OverviewPage />} />
          <Route path="/gallery" element={<GalleryPage />} />
          <Route path="/captures" element={<CapturesPage />} />
          <Route path="/capture/:id" element={<CaptureDetailPage />} />
          <Route path="/nodes" element={<NodesIndex />} />
          <Route path="/nodes/:id" element={<NodesPage />} />
          <Route path="/sounds" element={<SoundClassesPage />} />
          <Route path="/log" element={<BridgeLogPage />} />
          <Route path="/tests" element={<TestsPage />} />
          <Route
            path="*"
            element={
              <div className="page empty-line">
                Page not found. <Link to="/">Back to overview</Link>
              </div>
            }
          />
        </Routes>
      </main>
      {presenter && <Presenter onClose={closePresenter} />}
    </div>
  )
}
