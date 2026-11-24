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
| Älvsbyn SE | 50 Hz | STRONG nordic | STRONG nordic | −7 s | false fix, weak, 664 km off (moderate before fix 2) |
| Heath TX | 60 Hz | STRONG ercot | STRONG ercot | −3 s | – |
| Sunset Valley TX (34 edit points) | 60 Hz | STRONG ercot | STRONG ercot | −4 s | false fix, weak, 213 km off |
| Bristol GB | 50 Hz (weak) | none | none | – | – |
| Canterbury GB | 50 Hz (8 dB) | none | none | – | false fix, weak, 16 850 km off (blind) |
| Mannheim, Innsbruck, Gmunden, Iitti, Columbus, Bend | not detected | none | none | – | – |
| Oshkosh mass arrival | – | – | – | – | 0 flyovers detected |
| Oshkosh Sunday 3.7 h | – | – | – | – | killed at >3.2 GB RAM; after fix 1, 1.6 GB but stopped unfinished after 4 h |
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

## Problems found, and what changed

1. **Aircraft search memory grew with traffic in the window, not with the recording.**
   - The 3.7 h Oshkosh file has 1 detected flyover. Even so, `locate` (±1 day, Wisconsin
     bounding box, AirVenture week) was OOM-killed at 2.5 GB, and again at 3.2 GB, in the
     "time window" stage.
   - *Fixed:* `search_regions` now also cuts the start window into 4 h slices. A slice whose
     region loads more than 400 flight segments is halved, down to 10 min. Each slice loads
     tracks only around the flyovers' times
     ([architecture](aircraft/architecture.md#5-search-searchcpp)).
   - The same run now holds at 1.6 GB. Traffic peaks at ~800 segments in a 10 min slice.
   - *Still open:* run time. One flyover over AirVenture traffic is CPU-bound. The run was
     stopped after 4 h with half the 48 h window searched. A narrower window (`--from/--to`)
     or bounding box is the workaround.
2. **Over-confident aircraft fix from one event.**
   - In an indoor council meeting (Älvsbyn), a single detected "flyover" (fit 0.61 of a
     straight pass) was matched to a helicopter 664 km away and graded **moderate**. The
     hum-pinned ±3 s window leaves 3 candidate segments, so a 100% lead over the next
     combination means little.
   - *Fixed:* one flyover reaches moderate only with a fit of at least 0.8
     (`SINGLE_FLYOVER_MODERATE_FIT`, applied in `judge` and in the hum-and-aircraft fusion).
     Älvsbyn is now weak.
   - Colsto's two-flyover strong fix (80 m) is unchanged.
3. **False flyovers in indoor speech.** Council meetings yield 1–18 "flyovers" each. The
   detector is not retuned on this set, to avoid fitting the test.
   - *Mitigated:* every false fix is now graded weak.
4. **Misleading verdict reason.**
   - When every grid was scored but none had a consistent match, the verdict said "no reference
     data overlapped the search window", because `match::judge` counted only grids with a
     consistent best candidate.
   - *Fixed:* it now says "no grid matched consistently at any time in the search window". It
     keeps the old reason only when no grid had any candidate at all (Bristol).

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

## OpenSky check (2026-11-24)

The aircraft cases (`run.sh dated|blind`) with the check on (`GEOLOC_OPENSKY=1`, best 5
places, ±30 km), queried from a home connection, then again without credentials, reading
the answers back from the aircraft bucket.

| Recording | Checked | Outcome |
|---|---|---|
| Long Island security cam (dated) | 5 places, 10 hour windows (9 queries, 1 from the cache) | All 5 unchanged; the answer stays strong (N22NY + N93CQ, 80 m); OpenSky adds 1–4 segments per place, none within the silence range of the true place. From the bucket alone: the same. |
| Aberdeen (dated) | – | No place to check (no answer, as without OpenSky) |
| Älvsbyn, Canterbury (blind), Sunset Valley | – | Edited files: the per-clip search is not checked yet |

With `GEOLOC_OPENSKY=0` the new binary's reports equal the previous binary's on all five
cases, but for one Sunset Valley hypothesis whose segment count depends on which ADS-B days
an earlier run left on disk (the same for both binaries run in the same state).

Two faults the first live run showed, fixed before these results: OpenSky's query windows
are snapped outwards (10 min), and its tracks were read over the whole snapped span while
ADSB.lol's covered only the searched window, so a pass ADSB.lol was never asked for made
another flight pair win (now both are clipped to the same window and box); and "changed"
now needs a winner the search without OpenSky did not already have there.

## Aircraft detector on a labelled set: Livermore (2026-11-19)

Praveen Pathak's Livermore passages ([Zenodo 10.5281/zenodo.23424278](https://zenodo.org/records/23424278),
CC BY 4.0) are 40 smartphone recordings (85–115 s), each of one propeller aircraft passing one recorder in
May 2026. Each comes with its ADS-B track. The recorder's position is not published.
`scripts/eval/livermore/truth.py` fits it from the tracks and the published closest ranges:
37.66321, −121.78186, matching 37 of the 40 ranges to 3.5 m RMS (3 cases fall outside that fit and are marked
unreliable). Harness: `scripts/eval/livermore/run.sh download | events TAG | locate TAG`.

| Detector | Livermore passes found (CPA within 12 s) | ADS-B speed and slant inside the event's ranges | Other detections (40 single-pass clips) | Detections in the 11 council streams (no aircraft) |
|---|---|---|---|---|
| Before | **0 / 40** | – | 0 | 60 |
| Rising-template threshold, ridge check, same-source rule, speech mask | **28 / 40** | 20 / 28 | 6 | 15 |

Why the old detector failed:
- **Threshold.** It was set from the recording's own falling-template scores. In a short clip the one pass
  fills most instants and lifts the threshold above itself (R01: threshold 91, pass 66).
- **Speech.** Commentary lifts the same distribution: at Oshkosh, real flyovers scored 3–5 MAD and the
  threshold was 6.

The changes are described in [aircraft/architecture.md §2](aircraft/architecture.md#2-flyovers-in-the-recording-spectrogramcpp-eventscpp-ridgecpp).
The speech mask flags 50–94% of the council, Oshkosh and Aberdeen streams, 1% of colsto and 0–2% of Livermore.
colsto still gives exactly its two flyovers.

Still open:
- **Missed passes (12 of 40).** 1 is faster than the 180 m/s template limit (R50, 216 m/s). 3 are rejected
  by the ridge check (misfit 1.1–2.0 Hz, or the tone held in under 25% of the window). The others have no
  detection near the expected time. Truth times carry the phone clock's offset from ADS-B (up to ~8 s).
- **Speed and slant ranges miss the truth in 8 of the 28.** Mostly slow or close passes where the fit
  trades speed against distance (R41, R35, R36) or settles on an acceleration of ±2 m/s².
- **The end-to-end `locate` run on Livermore** (one flyover each, start known to ±15 s, town box) and a
  rerun of the YouTube set through `geoloc locate` have not been run yet.
