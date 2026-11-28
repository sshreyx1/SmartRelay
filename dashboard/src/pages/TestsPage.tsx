import { useCallback, useEffect, useRef, useState } from 'react'
import { Link } from 'react-router-dom'
import { testsApi } from '../lib/api'
import { clock } from '../lib/format'
import { usePolling } from '../hooks/usePolling'
import { useNow } from '../hooks/useNow'
import { Note, Panel } from '../components/ui'
import type { RunStatus, TestRun, TestSuite } from '../lib/types'

const STATUS_LABEL: Record<RunStatus, string> = {
  running: 'Running',
  passed: 'Passed',
  failed: 'Failed',
  cancelled: 'Cancelled',
  error: 'Could not run',
}
const STATUS_PILL: Record<RunStatus, string> = {
  running: 'pill is-warn',
  passed: 'pill is-ok',
  failed: 'pill is-bad',
  cancelled: 'pill is-muted',
  error: 'pill is-bad',
}

function StatusPill({ status }: { status: RunStatus }) {
  return (
    <span className={STATUS_PILL[status]}>
      <i className="dot" /> {STATUS_LABEL[status]}
    </span>
  )
}

/**
 * Runs the project's own automated test suites on this laptop and shows the output as it happens.
 * The backend only runs the fixed suites it lists (backend/suite_runner.py) and only for a page
 * served from this laptop.
 */
export function TestsPage() {
  const { data, error, refresh } = usePolling((s) => testsApi.suites(s), 'tests', 5000)
  const [run, setRun] = useState<TestRun | null>(null)
  const [lines, setLines] = useState<string[]>([])
  const [startError, setStartError] = useState<string | null>(null)
  const boxRef = useRef<HTMLPreElement>(null)
  const follow = useRef(true)
  const now = useNow()

  // pick up a run that is already going (page reloaded, or started from another tab)
  useEffect(() => {
    if (run == null && data?.current && data.current.status === 'running') {
      setRun(data.current)
      setLines([])
    }
  }, [data, run])

  // while a run is going: fetch new output every 700 ms
  const running = run?.status === 'running'
  const runId = run?.id
  useEffect(() => {
    if (runId == null || !running) return
    let after = 0
    let stop = false
    const tick = async () => {
      try {
        const v = await testsApi.status(runId, after)
        if (stop) return
        after = v.line_count
        if (v.lines && v.lines.length) setLines((old) => [...old, ...(v.lines ?? [])])
        setRun(v)
        if (v.status !== 'running') refresh()
      } catch {
        /* the next tick retries */
      }
    }
    void tick()
    const t = window.setInterval(() => void tick(), 700)
    return () => {
      stop = true
      window.clearInterval(t)
    }
  }, [runId, running, refresh])

  useEffect(() => {
    const box = boxRef.current
    if (box && follow.current) box.scrollTop = box.scrollHeight
  }, [lines])

  const start = useCallback(
    async (suite: TestSuite) => {
      setStartError(null)
      try {
        const r = await testsApi.run(suite.id)
        follow.current = true
        setLines([])
        setRun(r)
        refresh()
      } catch (e) {
        setStartError(e instanceof Error ? e.message : String(e))
      }
    },
    [refresh],
  )

  const suites = data?.suites ?? []
  const current = suites.find((s) => s.id === run?.suite)
  const elapsed = run ? (run.finished_at ?? now) - run.started_at : 0

  return (
    <div className="page">
      <div className="page-head">
        <div>
          <h1>Tests</h1>
          <p className="page-sub">
            Runs the project's automated tests on this laptop and shows the output live. Start with the live
            pipeline check: it takes about 10 seconds and its captures appear in the gallery, tagged SIMULATED.
          </p>
        </div>
      </div>

      {error && (
        <div className="error-line">
          Could not reach the test runner: {error}. It only answers a dashboard opened on the same laptop as the
          backend.
        </div>
      )}

      <div className="suites">
        {suites.map((s) => {
          const last = s.last
          const busy = running && run?.suite === s.id
          return (
            <section key={s.id} className={'panel suite' + (run?.suite === s.id ? ' is-current' : '')}>
              <header className="suite-head">
                <h2>{s.name}</h2>
                {last ? <StatusPill status={last.status} /> : <span className="pill is-plain">Not run yet</span>}
              </header>
              <p className="suite-proves">{s.proves}</p>
              <ul className="suite-cmds">
                {s.commands.map((c) => (
                  <li key={c} className="mono">
                    {c}
                  </li>
                ))}
              </ul>
              <footer className="suite-foot">
                <span className="muted small">
                  {last?.finished_at ? `Last run ${clock(last.finished_at)} · ${last.summary}` : s.duration}
                </span>
                {s.available ? (
                  <button className="btn btn-primary" disabled={running} onClick={() => void start(s)}>
                    {busy ? 'Running…' : 'Run'}
                  </button>
                ) : (
                  <span className="pill is-muted" title="Install it on this laptop to run this suite">
                    needs {s.missing.join(', ')}
                  </span>
                )}
              </footer>
            </section>
          )
        })}
      </div>
      {startError && <div className="error-line">{startError}</div>}

      <Panel
        title={run ? `Output: ${current?.name ?? run.suite}` : 'Output'}
        right={
          run && (
            <>
              <StatusPill status={run.status} />
              <span className="muted small">
                {elapsed.toFixed(0)} s{run.step && running ? ` · ${run.step}` : ''}
              </span>
              {running && (
                <button className="btn btn-ghost" onClick={() => void testsApi.cancel()}>
                  Cancel
                </button>
              )}
            </>
          )
        }
      >
        {!run && <div className="empty-line">Press Run on a suite above. Its output appears here as it runs.</div>}
        {run && run.summary && !running && (
          <p className={run.status === 'passed' ? 'run-summary ok' : 'run-summary bad'}>{run.summary}</p>
        )}
        {run && run.checks.length > 0 && (
          <ul className="checks">
            {run.checks.map((c) => (
              <li key={c.name} className={c.passed ? 'ok' : 'bad'}>
                <b aria-label={c.passed ? 'passed' : 'failed'}>{c.passed ? '✓' : '✗'}</b>
                <span>{c.name}</span>
                <small className="muted">{c.detail}</small>
              </li>
            ))}
          </ul>
        )}
        {run && run.suite === 'pipeline' && run.status === 'passed' && (
          <Note>
            The three transfers are now in the dashboard:{' '}
            <Link to="/gallery?data=sim">image gallery</Link>, <Link to="/captures?data=sim">captures</Link> and the{' '}
            <Link to="/log">bridge log</Link>. Open a capture to see its fragments, relays and CRC checks.
          </Note>
        )}
        {run && (
          <pre
            ref={boxRef}
            className="logbox runlog"
            onScroll={(e) => {
              const el = e.currentTarget
              follow.current = el.scrollHeight - el.scrollTop - el.clientHeight < 40
            }}
          >
            {lines.map((l, i) => (
              <div key={i} className={/^(PASS|ok)\b|passed|PASSED/.test(l) ? 'ok' : /FAIL|ERROR|error:/.test(l) ? 'bad' : ''}>
                {l || ' '}
              </div>
            ))}
          </pre>
        )}
      </Panel>

      <Note>
        Hardware tests (radios, sensors, five nodes) cannot run from here. Their procedures and recorded results are in
        docs/testing/ in the repository.
      </Note>
    </div>
  )
}
