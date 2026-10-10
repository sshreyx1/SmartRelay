/**
 * TBD: the old UI drew in-flight transfers as a fragment grid, filling in
 * live as the bridge's mesh radio received each piece. The real USB uplink
 * (common/uplink.h) only ever streams a transfer to the laptop once the
 * bridge has already fully reassembled and CRC-verified it -- there is no
 * per-fragment progress event on the wire today, so this panel has nothing
 * real to draw yet.
 *
 * To wire this up later: have the bridge's ALFP layer emit a progress line
 * over the same serial port as each fragment lands (it already tracks this
 * internally via alfp_rx_report_t), parse it in backend/live_bridge.py, and
 * push it over the existing WebSocket as a new event. The CSS for the
 * fragment grid (.fragments / .fragment / .progress-track) is already in
 * place from the old UI, unchanged, waiting for that data.
 */
export function LiveTransfers() {
  return (
    <section className="section">
      <div className="section-head">
        <h2>Arriving now</h2>
      </div>
      <div className="empty">
        <strong>TBD — live fragment progress isn't on the wire yet</strong>
        The bridge only reports a transfer to the laptop once it's fully reassembled, so there's
        nothing to animate in real time yet. See the comment at the top of this component's source
        for how to wire it up.
      </div>
    </section>
  )
}
