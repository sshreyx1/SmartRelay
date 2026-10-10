/**
 * TBD. The old UI's species taxonomy page grouped confirmed sightings by
 * mammal/bird/reptile/other with a per-species tally and drill-down. There
 * is no species classifier in the real pipeline yet (backend/classifier.py
 * in the old repo was a stub returning a fixed shortlist), so there is
 * nothing real to group here.
 *
 * To wire this up: once captures carry a species guess, add a
 * `/api/species` endpoint (tally + group breakdown, same shape as the old
 * repo's) and swap this placeholder for the grouped grid -- the
 * .species-grid / .species-card CSS from the old UI is already in
 * styles/app.css, unused until then.
 */
export function SpeciesPage() {
  return (
    <>
      <div className="page-head" style={{ marginBottom: 22 }}>
        <div>
          <h1>Species</h1>
          <div className="page-sub">TBD — no species classifier wired in yet</div>
        </div>
      </div>

      <div className="empty">
        <strong>Nothing to show here yet</strong>
        Species grouping needs an identification step the real pipeline doesn't have. Once a
        classifier is wired in (see the comment at the top of this page's source), confirmed
        species will group here by mammal / bird / reptile / other, same as the old UI.
      </div>
    </>
  )
}
