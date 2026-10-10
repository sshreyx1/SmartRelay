# SmartRelay dashboard

React + TypeScript + Vite. This is a UI port of the old trail-mesh demo
dashboard (same layout, same colour scheme, same components, down to the
CSS tokens) rebuilt against the real ALFP backend instead of a simulated
one. See the top-level `../README.md` for how this fits together with the
backend and real/simulated data.

```bash
npm install
npm run dev     # dev server on :5173, proxies /api, /captures and /ws to backend :8000
npm run build   # production build -> dist/
```

- `src/styles/tokens.css`, `src/styles/app.css` — unchanged from the old UI.
  Every class name (`.sighting`, `.station`, `.chip`, `.kpi`, …) is the same,
  so new pieces can be built with the existing design language instead of
  inventing new CSS.
- `src/lib/types.ts`, `src/lib/api.ts` — mirror the real backend's schema
  (`backend/app.py`, `storage.py`) field for field.
- `src/hooks/useLiveMesh.ts` — the WebSocket connection (nodes/captures/log/
  stats), plus notifications and an hour-of-day activity histogram derived
  client-side from real captures.
- `src/components/` — `TrailRail` (node sidebar), `CaptureFeed` +
  `CaptureModal` (capture cards + detail popup), `EnvironmentChart` +
  `Sparkline` (real temp/humidity/light trend per node), `MeshLog`,
  `NotificationCenter`, `ConnectionBadge`.
- `src/pages/` — `HomePage` (overview), `CapturesPage` (`/sightings`,
  paginated), `NodePage` (`/node/:id`), `SpeciesPage` and `ReviewPage`.

## What's TBD

A few things the old demo UI showed don't exist in the real pipeline yet.
Rather than fake them, the matching UI piece is kept (so the layout and CSS
are ready) and marked **TBD** in place, with a comment at the top of the
file explaining exactly what to wire up:

- **Species identification** (`CaptureFeed`, `CaptureModal`) — no
  classifier in the pipeline; every capture shows "Unidentified".
- **Review workflow** (`pages/ReviewPage.tsx`) — depends on the classifier
  above producing low-confidence guesses to queue.
- **Species taxonomy** (`pages/SpeciesPage.tsx`) — same dependency.
- **Live fragment progress** (`components/LiveTransfers.tsx`) — the real
  USB uplink only reports a transfer once it's fully reassembled; there's
  no per-fragment event on the wire yet.
- **RSSI / SNR / battery / duplicates / retransmit rounds** (`TrailRail`,
  `NodePage`, `CaptureModal`) — not carried over the current serial
  protocol between the bridge and the laptop.

Everything else — node status, captures, sensor readings, trigger, radio,
weather, sound class, the mesh log, and the environment trend chart — is
real data from the backend.
