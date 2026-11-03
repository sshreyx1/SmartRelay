# H9: dashboard in a browser with the simulator (2026-11-03, R's screenshots)

- `demo_feed.py --burst 5/3 --label-images sd_card/samples` → `POST /api/demo/ingest 200` every time.
- Home / All captures / node page / capture modal: `N2-0000xx` badge, `CAT · 52% · MAMMAL` and
  `INSECT_FROG (unidentified) · 80%` chips, "identified on-node", picture matches the label (cat, pig, cow, frog).
- Species page: empty with "include simulated captures" off (all rows are demo), with it on: Cow 1, Cat 1, Pig 1,
  Insect/frog (unidentified) 3, Frog 1, grouped MAMMAL / INSECT / FROG.
- Bugs found and fixed: WS snapshot rows had no `image_url` (no images after a reload, pre-existing since 2e89e98);
  the simulator's no-label placeholder was undecodable random bytes; simulator ids restarted at 1 per run.
- Not covered here: a real capture through the bridge (H8).
