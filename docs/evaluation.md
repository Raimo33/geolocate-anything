# End-to-end evaluation on real recordings (2026-11-19)

Fourteen past YouTube livestreams plus one security-camera recording were run through
`geoloc locate`, the same command the dashboard runs on an upload. References came from the
grid bucket and ADS-B days from the aircraft bucket, as on the deployed server. Ground truth
for the time is YouTube's `release_timestamp` (when the livestream started). Ground truth for
the place is the town, or the airfield for aircraft.

Harness: [`scripts/eval/`](../scripts/eval) (`run.sh download`, `run.sh MODE ID...`,
`score.py`; recordings and truth in `truth.tsv`).

## Test set (easy → hard)

| Tier | Hum (grid) | Aircraft |
|---|---|---|
| A | Council meetings in Mannheim, Innsbruck, Gmunden (continental Europe, 2–5 h) | Oshkosh AirVenture 2026, Cessna mass arrival, runway 36 (44 min) |
| B | Bristol, Canterbury (GB); Älvsbyn (SE), Iitti (FI) (Nordic) | Oshkosh, Sunday arrivals (3.7 h); colsto's Long Island security camera (1 h, surveyed truth) |
| C | Columbus OH (US East), Bend OR (US West), Heath and Sunset Valley TX (ERCOT) | Aberdeen airport spotting stream with chat (4 h, helicopters and turboprops) |

Modes: **dated** searches ±1 day around the true start (a user who knows the day), **blind**
searches all history (no date given), and **forced50/60** is dated with the hum frequency forced
(the dashboard's 50/60 Hz option), run only where detection failed. Audio is YouTube's best
stream (Opus ~128 kbit/s, 48 kHz).

## Results

| Recording | Hum | Dated | Blind | Start error | Aircraft |
|---|---|---|---|---|---|
| Long Island security cam | 60 Hz | STRONG us_eastern | STRONG us_eastern | −1 s | STRONG, **80 m** from truth (2 flyovers) |
| Älvsbyn SE | 50 Hz | STRONG nordic | STRONG nordic | −7 s | false fix, *moderate*, 664 km off |
| Heath TX | 60 Hz | STRONG ercot | STRONG ercot | −3 s | – |
| Sunset Valley TX (34 edit points) | 60 Hz | STRONG ercot | STRONG ercot | −4 s | false fix, weak, 213 km off |
| Bristol GB | 50 Hz (weak) | none | none | – | – |
| Canterbury GB | 50 Hz (8 dB) | none | none | – | false fix, weak, 16 850 km off (blind) |
| Mannheim, Innsbruck, Gmunden, Iitti, Columbus, Bend | not detected | none | none | – | – |
| Oshkosh mass arrival | – | – | – | – | 0 flyovers detected |
| Oshkosh Sunday 3.7 h | – | – | – | – | **killed: >3.2 GB RAM** |
| Aberdeen | – | – | – | – | weak, 47 km off, wrong time |

In short:

- **No wrong grid or wrong time across 27 runs.** Each of the 5 hum verdicts is STRONG, in the
  right grid and within 7 s of the true start, blind as well as dated. Blind runs take 2–6 min
  (17 min for the 34-segment Sunset Valley file), against 0.5–2 min dated.
- **Hum is the bottleneck, not matching.** 6 of 11 council streams have no detectable hum. Forcing
  50/60 Hz gives no match either (and no false match), so the hum is missing, not missed by a
  detector set too strict. Innsbruck and Gmunden come closest: ridge test p = 0.003 and 0.005,
  against a threshold of 0.001. Modern council AV (digital mixers, high-pass filters, noise gates)
  appears to remove it.
- **Aircraft on its own works only with clean ambient audio.** The security camera is located to
  80 m. Streams with commentary (Oshkosh, Aberdeen) have either no detectable flyovers or too few
  to agree.

## Problems found

1. **Aircraft search memory grows with traffic in the window, not with the recording.** The
   3.7 h Oshkosh file has 1 detected flyover, yet `locate` (±1 day, Wisconsin bounding box,
   AirVenture week) is OOM-killed at 2.5 GB and again at 3.2 GB in the "time window" stage.
   The first 45 min (0 flyovers) need 0.7 GB.
2. **Over-confident aircraft fix from one event.** In an indoor council meeting (Älvsbyn), a
   single detected "flyover" (fit 0.61 of a straight pass) is matched to a helicopter 664 km away
   and graded **moderate**: the hum-pinned ±3 s window leaves 3 candidate segments, so "lead over
   the next 100%" means little. A single event with a poor fit should cap at weak.
3. **False flyovers in indoor speech.** Council meetings yield 1–18 "flyovers" each. The ones
   that reach a fix are graded weak, except item 2.
4. **Misleading verdict reason.** When every grid was scored but none had a consistent match,
   the verdict says "no reference data overlapped the search window" (`match::judge` counts only
   grids with a consistent best candidate). It should say that no grid matched consistently.

## Reproducing

```bash
YTDLP=/path/to/recent/yt-dlp scripts/eval/run.sh download
railway run -- scripts/eval/run.sh dated  $(cut -f1 scripts/eval/truth.tsv | tail -n +2)
railway run -- scripts/eval/run.sh blind  L6cxayreMCY OO3bswY_8Nw SO2qZYPF8Ck ksy9WzjQuJI R2t_bV5j5dM
scripts/eval/score.py
```

The colsto recording (`colsto_4CxVxuMVkMc`, YouTube 4CxVxuMVkMc) is not a livestream and has no
`release_timestamp`; its truth is DOPPLERGEIST's published start. `download` skips it: copy
`~/.cache/geolocate-anything/wild/colsto_4CxVxuMVkMc.webm` into `$EVAL_AUDIO` and write an
`info.json` with that start to include it.
