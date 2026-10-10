/**
 * TBD. The old UI's review queue listed low-confidence classifier guesses
 * for a person to confirm or dismiss (see the old SightingModal's
 * review-box). Without a classifier there's no "low confidence" signal to
 * queue up -- every real capture here is already a verified, complete
 * transfer, just not identified.
 *
 * To wire this up: once captures carry `needs_review`/`human_reviewed`
 * (from a classifier) and a `/api/captures/{id}/review` endpoint exists,
 * swap this placeholder back for CaptureFeed filtered to needs_review --
 * the .review-box / .review-select CSS is already in styles/app.css.
 */
export function ReviewPage() {
  return (
    <>
      <div className="page-head" style={{ marginBottom: 22 }}>
        <div>
          <h1>Review queue</h1>
          <div className="page-sub">TBD — no review workflow wired in yet</div>
        </div>
      </div>

      <div className="empty">
        <strong>Nothing to review yet</strong>
        There's no classifier producing low-confidence guesses to queue up. See the comment at the
        top of this page's source for how to wire the review workflow back in.
      </div>
    </>
  )
}
