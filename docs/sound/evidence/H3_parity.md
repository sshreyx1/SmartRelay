# H3: Pico features = laptop features (2026-11-03, R's run)

`python ml/check_parity.py --takes 5` with audio_dump.uf2 on node 2; takes: talking, claps, quiet, whistle/hum,
the dog clip from the speaker. Max relative difference per take: 5.98e-08, 1.13e-07, 1.96e-07, 0, 1.03e-07
(tolerance 1e-3). Worst feature: loud_std 1.96e-07; 11 of the 25 features identical bit for bit.
Verdict: **PASS** (float32 rounding only).
