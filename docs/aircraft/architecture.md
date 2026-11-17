# Aircraft Doppler

How geolocate-anything turns the faint drone of a passing propeller aircraft into a
place, and how that pairs with the grid hum. The idea follows colsto's demonstration
(the video locates a security-camera recording from two flyovers); the implementation
is our own (C++, `src/air/`). [DOPPLERGEIST](https://github.com/1337isnot1337/DOPPLERGEIST),
an independent Python reconstruction of the same video, was read for its documented
data sources and pitfalls only: it is PolyForm-Noncommercial and none of its code is used.

```
recording ──► whitened spectrogram ──► flyovers (CPA time, speed, slant range,
  (4 kHz)       (log power − baseline)     rest frequency + harmonics, as ranges)
                                                    │
ADSB.lol day ──► non-jet tracks ──► corridors ──► one clock ──► silence ──► exact curves ──► place + start time
  (~4 GB tar)     (~48 MB/day)      (200 m cells)  (all events   (no unheard   (real trajectory,
                                                    heard at the   flight in     emission time solved,
  Copernicus DEM + EGM96 ─────────────────────────► right gaps)    between)      ridge score)
```

## 1. Physics (`src/air/doppler.cpp`)

A listener at `o` hears, at time `t`, the sound emitted at `tau` with
`t = tau + |x(tau) − o| / c`, at frequency `f0 · c / (c + u_r(tau))`, `u_r` the radial
velocity at emission (positive receding). The emission time is solved for every
reception time (Newton on `tau + r(tau)/c − t`, 2–4 steps). Using the radial velocity at
`t` and delaying the curve as a whole gives a differently shaped curve, not a shifted
one: 30 s after the CPA of a 120 m/s pass at 1.5 km the two differ by 0.6% (0.7 Hz at
113 Hz), more than the scoring tolerates.

For a straight constant-speed pass the equation has a closed form (`cv_ratio`): with
`T` the time since the closest-approach emission,
`s = (c²T − sqrt(c²v²T² + d²(c² − v²))) / (c² − v²)` is the emission time and the shape
depends on speed `v` and closest slant distance `d` only. Trajectories from ADS-B are
cubic Hermite curves through the reported positions and velocities (finite differences
where a reported velocity is missing or disagrees with the positions), in ECEF, never
extrapolated across a gap.

`c` is one effective speed (340 m/s, `--sound-speed`). Wind, refraction and terrain
shadowing are not modelled; their effect is absorbed by the margins and sweeps below.

## 2. Flyovers in the recording (`spectrogram.cpp`, `events.cpp`)

| Step | Choice | Why |
|---|---|---|
| Spectrogram | 4 kHz audio, 0.512 s Hann frames, 0.128 s hop, 20–1200 Hz | Propeller blade-pass tones and their harmonics sit at 60–600 Hz; 1 Hz bins. |
| Whitening | log power − per-bin median over 45 s blocks − per-frame median, ÷ per-bin MAD; clipped to [−3, 8] | Removes stationary tones (mains hum, fans), the noise colour and broadband gusts; a moving tone stands out by a few units, and sums along a curve behave like sums of unit-variance noise. |
| Detection | A straight pass is one fixed shape in log frequency per (v, d), moved up or down by log f0. Templates over v 30–180 m/s × d 120–9000 m are summed along a pooled (0.5 s, 0.25%) log-frequency map, for every rest frequency at once, single line or a three-harmonic comb | One matched filter for every rest frequency is a sum of shifted rows: an hour is scanned in ~10 s on 12 cores. |
| Threshold | median + 6 MAD of the per-instant best score (and ≥ 6) | The recording's own score distribution; clutter (boats, cars, birds) passes too and is ignored by the search when no aircraft explains it. |
| Measurement | 4-D grid (CPA time, v, d, f0) on the full-resolution spectrogram; ranges = every point within max(3, 15%) of the best score; other lines at simple ratios of f0 (½, 2, 3, …) kept when they score ≥ 30% | Aircraft turn and change speed: ranges, not points, go to the search. |

On colsto's recording (one hour, Opus from YouTube) the two flyovers the video uses come
out as the two strongest detections: CPA at 2154.4 s, 124–156 m/s, 4.5–8.5 km, 113.8 Hz
and its harmonic; CPA at 3075.8 s, 94–126 m/s, 2.2–5.2 km, 130.2 Hz. 921.4 s apart. The
video's hand measurements were 130–154 m/s, 4.3–7.8 km and 88–110 m/s, 1.4–4.7 km, about
920 s apart.

## 3. Flight history (`adsb.cpp`)

| Decision | Why |
|---|---|
| **ADSB.lol daily archives** (`adsblol/globe_history_<year>` releases, ODbL) | Free, complete days of every aircraft the network saw, readsb `trace_full` JSON per aircraft, from 2023-03 on. Prod release first, then staging. Assets are found by name (`<tag>.tar`, or parts `.tar.aa`, `.tar.ab`, ...), not through the rate-limited GitHub API; a dropped download resumes with a byte range. |
| **Streamed, never stored raw**: tar parts → tar reader → gzip members → worker threads | A day is ~3.9 GB in two parts. 42 s for a whole day on the laptop's 12 threads, ~1 GB peak, constant in the day's size. `--parts` reads local copies. |
| **Jets dropped before parsing**: the type designator is read from the first 512 inflated bytes | Jets are half the traces and most of the bytes. |
| **Missing types from tar1090-db's registry** (`aircraft.csv.gz`, 645k aircraft) | 40% of traces carry no type. Before this, two of the ten best wrong answers on the reference case were business jets (an Embraer Phenom 300, a Citation XLS) of unknown type. Unknown types are still kept (`--known-types` drops them). |
| **Altitude above the ellipsoid**: GNSS altitude where reported; else barometric + that aircraft's median geometric−barometric offset; else + the median offset of all aircraft in its 2° × 2° × 3 h cell | Pressure altitude is off by the day's pressure deviation (hundreds of feet). On 2026-09-08: 34.4M geometric fixes, 0.24M corrected per aircraft, 1.25M per region, 0.02M left raw. |
| **Store** (v3): one file per day, per-aircraft zstd blobs of *trajectory nodes* + an index (aircraft, type, bounding box, time span) zstd-compressed at the end | A search reads the index and decompresses only aircraft whose box and time overlap. Fixes are not kept: the search reads a flight as a cubic Hermite curve through positions and velocities, so the store keeps that curve's nodes. Spikes (bad decodes, baro glitches) are dropped, each fix is smoothed (quadratic least squares over ±6 s, positions and reported velocities), and from each node the next is the farthest fix whose curve stays within 25 m horizontally, 15 m vertically and 5/3 m/s of every smoothed fix between (≤ 100 s apart; leg starts and gaps always split). Nodes sit on whole seconds; velocity (0.5 m/s) and position (5e-5°, 4 m) are coded as integer residuals from the previous nodes (velocity extrapolated, position moved by the trapezoid of the velocities) in 16-bit byte planes, zstd 19: 5.7 B a node. 2026-09-08: 36.9M fixes → 8.5M nodes, **48 MB** (v1 0.68 GB, v2 ~0.22 GB); 2026-11-12: 33.0M fixes, 41 MB. Against the raw fixes the curve is within 11 m (median) / 31 m (95%) and 0.7 / 5 m/s radial velocity: the raw fixes' own disagreement between reported velocity and position. Coarser tolerances gain little (50 m: −20%); larger zstd frames 5%, velocity from node positions instead of stored: worse. Older stores are not read (a volume's are deleted and fetched again). |
| **Chunk index** (sidecar `adsb/<day>.idx`, not a store version): per aircraft, its nodes in 10-minute slots, each slot's box (0.01°, rounded outwards) and node speed range (1 m/s); columnar, zstd 19 | `load_tracks` decompresses an aircraft only if a slot within 120 s of the window has a node in the box (the segments it would yield need two such nodes 120 s apart at most, so the result is the same), and, given a speed range, a node at that speed. Built from the store alone: at ingest, on first use when missing or stale (it records the store's size and tail), or with `geoloc aircraft index [DAY...]`, so the bucket's ~1,350 v3 days need no rebuild and no new download (3 s a day; 1,350 days ≈ 70 min on 4 threads, or lazily as days are used). 2026-09-08: 35,129 aircraft, 448,527 slots, **2.8 MB (5.8% of the store)**. Not uploaded: a server rebuilds it the first time a day is used. Aircraft decompressed, before → after: the US in 3° regions over a whole day 84,265 → 61,429 (73%); the US Eastern extent around the hum's second 6,733 → 5,252 (78%); Long Island then 494 → 278 (56%). With the event speeds (72–178 m/s) as well, about 42% of the region loads remain, but the search does not filter by speed when loading: the silence stage needs aircraft of every speed (it skips the others itself, by their segment bounds). Loading is a few seconds of a large search either way. |
| **Aircraft bucket** (`GEOLOC_AIRPLANE_BUCKET`, apart from the grid bucket): `adsb/<day>.trk` + `adsb/<day>.cov` | All of ADSB.lol's history (~1,350 days, ~65 GB) is built offline (`aircraft fetch 2023-03-01..YESTERDAY --push --drop`, ~3 min a day, download-bound) and kept there. A server's volume holds every day's coverage sidecar (pulled on start, for the map) and only the stores its analyses used last (`GEOLOC_AIRPLANE_KEEP_DAYS`), each downloaded on first use. The dashboard's **Fetch the latest data** (admin, `POST /api/adsb-refetch {"latest": true}`) fetches whichever of the last 7 UTC days are missing and uploads them (`{"day": D}` still rebuilds one day with `--force`). |
| **Segments**: split at readsb leg starts and gaps > 120 s | No interpolation across gaps. |

## 4. Terrain (`terrain.cpp`)

Slant distances need the listener's height in the aircraft's datum. Copernicus GLO-90
(1° COG tiles on a public bucket, heights above EGM2008) + EGM96 undulation from PROJ's
15' grid (differs from EGM2008 by ~1 m). Tiles are downloaded on first use and cached; a
tile absent from the bucket is sea. The corridor stage uses 1/40° block means (a few kB
per tile, lock-free); scoring uses the 90 m DEM (a 32-tile cache). A small float-TIFF
reader (deflate, floating-point predictor, tiled or stripped) avoids a GDAL dependency.

## 5. Search (`search.cpp`)

| Stage | What | Cost on the reference case (Long Island box, whole day) |
|---|---|---|
| Rest-frequency prior | A segment stays a candidate for an event only if its type can make the event's tone: blade pass (RPM × blades / 60) or, for direct-drive pistons, firing frequency, over the type's RPM range from descent to take-off, the event's f0 tried as ½, 1, 2 or 3 × that, ± 4% ([blade-pass.md](blade-pass.md)). Types not in the table (unknown, helicopters, geared engines) always stay. One aircraft explaining two events must have agreeing tones (± 5%, or a simple ratio). | see below |
| Corridors | Per event, each flight segment flying within the event's speed range (± 12 m/s) in the window is cut in 4 s pieces; each piece writes its closest slant distance into the 200 m cells of its slab (cells whose foot falls in the piece, within reach of the slant range). Per (segment, cell), the minimum; minima at a segment end are not closest approaches. With ≥ 2 flyovers required, **pair-first** (below) rasterizes only where pieces of enough events can meet. | 3,042 segments → 358k cells, 62 cpu-s (pair-first: 161k cells) |
| One clock | Each (cell, segment) entry implies an interval for the recording's start (CPA reception time − the event's CPA time ± its range ± 4 s). A cell survives if entries for ≥ `min_events` events share an interval. Subsets of the events are tried: clutter stays unmatched. | → 54k cells |
| Silence | Another aircraft passing closer than the matched flyovers' own slant range, between the first and last matched flyover, at a time no flyover was detected, would have been heard: the cell goes. | → 12k cells |
| Exact curves | Each matched segment's curve is modelled for the listener (emission time solved), tabulated against absolute time, and scored against the spectrogram: one start time swept for all events, each event's rest frequency swept on its own (± 2%). A quick brightness fit ranks every survivor, a full fit the best 1,500 at the cell centre, then the best 300 on a 100 m sub-grid; the final fine pass scores on ridges. | 88 cpu-s + 12 s |
| Ranking | Best point per combination of flights; the leading five refined on a 20 m grid. | 11 s |

A combination of flights usually survives in a corridor of thousands of contiguous
cells whose scores vary smoothly: the quick fit takes every third cell each way, then
the 5 × 5 neighbourhoods of the best four (centre scoring 88 → 23 cpu-s on the
reference case, same answer).

### Large areas: pair-first corridors and segment bounds

Over a day and a continent nearly every cell lies in some event's corridor (181M
cells for the US), but a survivor needs entries of ≥ `min_events` events whose start
intervals meet, and the recording fixes the gaps between those events' CPAs. So, per
0.25° tile and before rasterizing:

1. **Pieces as intervals and footprints.** Each 4 s piece within reach of the tile gets
   the start interval any of its entries could imply
   (`[ta + d_lo/c − tc_hi − 4 s, tb + d_hi/c − tc_lo + 4 s]`, clipped to the window) and
   the rows and longitudes of the cells it can write.
2. **Pairs.** Pieces of two events are partners when their intervals overlap and their
   footprints share rows and longitudes (a sweep over the intervals sorted by start).
3. **Pruning.** A piece needs kept partners in ≥ `min_events` − 1 other events, else it
   is not rasterized; repeated until stable (a dropped piece is no one's partner). This
   holds for every subset of the events the one-clock stage tries, so clutter stays
   unmatched as before.
4. **Masks.** A kept piece writes a cell only if kept partners of ≥ `min_events` − 1
   other events also cover it (the same row/column test the rasterizer uses; skipped
   above 64 partners, where it would cost more than it saves).
5. **Exact entries.** Dropping pieces can only raise a (segment, cell) minimum, so in
   every cell that still holds entries of ≥ `min_events` events each entry is
   recomputed over all of its segment's pieces. The piece that holds a combination's
   true minimum always has the combination's other pieces as partners covering that
   cell, so every surviving combination is found with its exact entry, and no other.

The one-clock, silence and scoring stages therefore see the same cells and entries as
before (the unit test checks cells, places, starts and scores against the full
rasterization). With one flyover (the hum's second) every piece stands alone and
the stage is skipped.

**Segment bounds.** From its nodes alone each segment gets a box that holds its whole
Hermite curve (within (4/27)·h·(|v₀| + |v₁|) of each chord) and bounds on its speed
(the velocity is the interpolated node velocity ± 1.5 |chord velocity − mean node
velocity|). Segments never at an event's speed are not cut into pieces at all, and the
silence stage cuts only segments whose box comes within reach of the tile; it also
rasterizes only the columns of each row that hold survivor cells. All three skip only
work that changes nothing (`segment_bounds = false` turns them off, as the test does).

Measured (4 threads; ADS-B of 2026-09-08; offline, so ground at the geoid; before the
rest-frequency prior was merged). colsto's audio was not reachable from this
environment, so the recording is synthetic: one hour rendered at the reference place
(40.87621, −73.52335, start 20:17:30 UTC) from the real tracks of N22NY (113.8 Hz +
harmonic) and N93CQ (130.2 Hz) in white noise; the detector finds their CPAs at
2157.3 s and 3076.0 s (the real recording's 2154.4 and 3075.8 s) as the two strongest
of 12 detections, the others side-lobes of those two, which serve as clutter. "Same"
means all 20 candidates (place, start, flights per event) are identical; the funnel
after the corridors is identical in every case.

| Case | Before | After |
|---|---|---|
| Synthetic unit test (two flyovers, decoys; with the prior) | 75,866 corridor cells → 386 → 386 | **8,665** (2,050 of 4,047 pieces) → 386 → 386; every later count, place, start and score bit-identical (the test checks) |
| Long Island box (−74.2..−72.5, 40.5..41.4), whole day, 2 flyovers | 3,385 segments → **356,924** corridor cells → 63,467 one clock → 16,095 silent; corridors + silence 13 cpu-s; 2:08, 0.98 GB | **161,231** cells (72k of 280k pieces rasterized) → 63,467 → 16,095; 7 cpu-s; 2:06, **0.60 GB**; same |
| Same, ± 2 h, 3 events (one clutter) | 356,924 → 95,888 → 64,981; 10 cpu-s; 6:35, 0.68 GB | 295,512 → 95,888 → 64,981; 9 cpu-s; 6:26, 0.54 GB; same, clutter unmatched |
| Northeast (−80..−68, 38..47, 12 regions), whole day | 27,397 segments → **17.5M** → 379,620 → 206,794; corridors + silence 114 cpu-s; 14:23, 1.07 GB | **1.26M** (11% of 3.4M pieces) → 379,620 → 206,794; 34 cpu-s (pairing 3); 14:19, **0.62 GB**; same (the reference place #1, lead 3%) |
| US Eastern extent (−98..−55, 24..55) at the hum's second ± 3 s, 1 flyover | 8,528 segments → 250,018 cells; corridors 9 cpu-s; 4:47, 0.59 GB | pair-first not used (one flyover); segment bounds: 1 cpu-s; 4:44, 0.55 GB; same (strong) |
| Continental US (−125..−66, 24..50), whole day, 2 flyovers (after: also with the rest-frequency prior, which alone removes ~15% of the one-clock cells) | 176,029 segments → **181.7M** → 1.55M → 996k; corridors + silence 562 cpu-s, centre scoring 12,459 cpu-s; **1 h 06 min, 1.04 GB**; the reference place #1 (the documented run on the real audio, 12 threads: 181M → 1.4M → 0.86M, 2 h 41 min, 2.2 GB) | 176,029 segments → **5.41M** (1.8M of 21.6M pieces) → 1.32M → 835k; corridors + silence 147 cpu-s (pairing 12, silence 34), centre scoring 10,934 cpu-s; **57 min on 4 threads, 0.67 GB**; the reference place #1 (lead 3%: this clean synthetic audio fits better than the real, which ranked it #3) |

What remains is centre scoring (the quick fit of every survivor: 2,700 of the
Northeast's ~2,850 cpu-s, 10,900 of the continental run's ~11,100, 995 of the
hum-second run's), untouched here. Its cost per
cell depends on the events' scoring windows (this recording's reach the 60 s cap), so
on the real audio, where the documented Long Island run spent 23 cpu-s on 12k cells,
the corridor and silence stages are a larger share. Loading tracks is small either way
(0.4 s for the Long Island region with its index, 1.9 s for the 12 Northeast regions).

Large boxes are searched in 3° regions, one at a time (memory stays near 2 GB for the
whole US). Each region's refinement budget is the global one divided by the number of
regions (at least 100 full fits, 30 sub-grid places, 2 refinements), and a region's
survivors must come within 60% of the best answer found so far. A time window under
6 h (a hum second) is searched in one pass over the whole box instead: few tracks
fly in it even over a continent, and the tiles then run in parallel.

### Rest-frequency prior: measured

| Case | Without (`--no-type-prior`) | With |
|---|---|---|
| Synthetic unit test (4 segments; the two decoys are a C152 mimicking the PC-12's 105 Hz pass and a PC-12 mimicking the C172's 85 Hz pass) | 81,769 corridor cells → 660 one clock → 660 silent; 14.3 s; true pair #1, lead 43%; the decoy pair #2 | 5 of 24 (segment, event) pairs dropped; 75,866 → 386 → 386; 10.6 s; true pair #1, lead 44%; the decoy pair gone |
| Reference case's ADS-B (2026-09-08, Long Island box −74.2..−72.5, 40.5..41.4, whole day), the two measured events (113.8 + 227.6 Hz, 130.2 Hz) on a noise recording (the original audio was not available to that run, so places are not scored and only the funnel counts) | 3,317 segments → 356,924 → 48,360 → 8,060 cells; 35.1 s (4 threads) | 847 of 6,634 pairs dropped (13%), 40 segments for both events; 356,924 → 45,056 (−7%) → 7,104 (−12%); 34.0 s. N22NY (PC12) + N93CQ (B350) survive: in a small box around the reference place they are the #1 combination, with or without the prior |
| Same, northeastern US (−78..−70, 38..44) | 19,420 segments → 8.38M → 200,791 → 74,413 cells; 111.5 s | 4,600 of 38,840 pairs dropped (12%); 8.29M → 188,637 (−6%) → 70,295 (−6%); 110.5 s |

The prior is cheap and safe but prunes little at these tones: 114 and 130 Hz sit inside
the range of most piston singles (a six-cylinder engine fires at 3× shaft rate: 95–143
Hz) and of many turboprops, and types the table does not hold (or unknown) are kept. It
removes look-alikes at tones far from a type's band (a 105 Hz pass explained by a
two-blade four-cylinder Cessna, an 85 Hz pass by a PC-12) and matters more for
flyovers below ~90 Hz (airliner turboprops, slow pistons) or above ~150 Hz.

Two scoring choices were measured on the reference case:

* **Brightness saturates on strong tones**: summing z along the curve cannot tell a
  curve 0.2 Hz off the ridge from one 1.5 Hz off (the 0.5 s window's main lobe is
  ±2 Hz). The final pass therefore uses *ridge* evidence: the strongest bin within 3σ of
  the model frequency, its frequency refined by a parabola, weighted by
  `exp(−½ (Δf/σ)²)` with σ = max(0.3 Hz, 0.4% of f).
* **One clock**: the start time is shared; event 2 cannot slide to fit on its own (it
  falls where the flight geometry puts it), as in the video.

Longer scoring windows (×1.5) and 1 s frames were tried and did not separate the true
answer further (lead 1–6% vs 7% for the defaults at the time).

### Verdict

`fit` = an event's score on the real trajectory ÷ its score along the best straight
pass. **Strong**: ≥ 2 flyovers, every fit ≥ 0.7, lead over the next flight combination
≥ 20%. **Moderate**: fits ≥ 0.5, lead ≥ 10%. Else weak.

### Silence over the whole recording: rejected

Applying the silence test to the whole recording (an hour) instead of between the
matched flyovers removed the true place and made a wrong one *strong*: it depends on the
detector having found every audible flyover, and it had not (only the two strongest were
above the default threshold). Between the flyovers, the video's rule, is the default;
`--silence-whole` remains for experiments.

## 6. With the grid hum (`fusion.cpp`, `locate.cpp`)

`geoloc locate` runs the aircraft stage after the hum when ADS-B history is stored
(`--aircraft auto|on|off`, `--aircraft-fetch` to download missing days,
`--aircraft-bbox` for an area without a hum match):

| Hum result | Aircraft search | What it adds |
|---|---|---|
| Located (moderate/strong) | At the hum's second ± 3 s, over the matched grid's extent (candidates outside the grid's polygon dropped); one flyover suffices | A place within a few hundred metres instead of a continent. |
| Weak or none, with candidates | The best five hum alignments (all grids) as hypotheses, ± 3 s each, two flyovers required | A hum alignment that an aircraft match confirms is right for both; the others fall. |
| No hum / no candidate | The `--from/--to` window over `--aircraft-bbox`, two flyovers | Place and start time; then the hum is re-searched in the grid of that place, ± 6 h around the aircraft's time, and the rank of the alignment within 3 s of it is reported. With ~43,000 lags instead of years of them, a faint hum that could not stand out globally can confirm the time. |

### Edited files (`search_clips`, `run_aircraft_clips`)

When the hum finds edit points (README "Edit detection"), the file joins clips recorded
at different times, in an order that may have been rearranged. The aircraft stage then
works per clip, sharing only the place:

| Step | Choice | Why |
|---|---|---|
| Flyovers | A whitened spectrogram and a detection per clip (clips under 60 s skipped) | No template or whitening block straddles a cut; times are the clip's own. |
| Clocks | One start window per clip and hypothesis: a located clip's hum second ± 3 s (one flyover suffices), else its best three hum alignments ± 3 s, or `--from/--to` (two flyovers) | Clips' clocks are unrelated: nothing assumes contiguity. The silence test runs within a clip, between its own flyovers. |
| Alone | Each clip's hypotheses searched as an unedited recording, over the hum grid's extent or the given area | Each clip's own candidate places. |
| Places to try | Each clip's best five answers; for a one-flyover answer also its mirror image across the aircraft's track at closest approach | A single straight pass draws the same curve on either side of its track: in the synthetic test both clips alone chose the wrong side (1.8 and 3.6 km off). |
| At each place | Every clip searched again within 1 km of it, on its own clock; the place's evidence is the sum of the clips' best scores | The other clips decide between the strips' sides and between flights. |
| Joint point | Around the best place, a 50 m then 12 m grid (± 200 m, ± 50 m): at each point every matched clip's clock is fitted (± 1.5 s, its matched flights' exact curves, ridge scores) and the scores summed | Alone, a one-flyover clip trades its clock against position along its aircraft's track (100 m ↔ 1 s); the clips' tracks cross at one point. |
| Verdict | As for one clock, over all clips: every flyover explained (fits), lead of this place's evidence over the best place more than 1 km away. A clip that alone located (moderate or better) more than 1 km away is flagged and caps the verdict at moderate | The camera may have moved, or one answer is wrong. |

The report's `aircraft` keeps one list of flyovers, hypotheses, flights and spectra in
file time (each tagged with its `clip`; a spectrum's `utc0` is UTC at file time 0 on its
clip's clock), plus `clips` (per clip: span, start time at the shared place, matched
aircraft, the hypothesis that gave the time, its own place's distance) and `shared` (the
place, verdict and reasons). The dashboard's aircraft tab lists each clip's start time
and aircraft above the flights.

## 7. Dashboard

When a result involves aircraft, the dashboard's result view gains an **Aircraft** tab
and the map zooms to street level (a gray basemap fades in from zoom 7):

- the place (pulsing pin), each matched aircraft's track ± 10 min, a plane icon at its
  closest approach pointing along its heading, and a dashed line to the place;
  runners-up as grey dots with their flights and scores;
- hovering a track, plane or card highlights it; clicking opens the aircraft's details:
  registration, type and description, build year, engines, operator (tar1090-db),
  ICAO address, closest-approach time and slant distance, altitude, speed, heading,
  the tone's rest frequency and fit, and a link to its track on adsb.lol;
- each flyover's spectrogram (the whitened one the search scores on, around the CPA,
  covering every line's Doppler sweep), with the curve modelled from the aircraft's
  real track (dashed) and the straight-pass measurement (dotted); hover reads out time
  in the recording, UTC and frequency, click hides the curves. Detections no aircraft
  explained are listed below.

The **History** page has an **Aircraft history** switch: a separate overlay layer (its own
map pane above the grids' coverage, which dims beneath it) colouring each 0.5° cell red
to green by the aircraft-minutes of ADS-B history stored there (distinct minutes an
aircraft had a fix in the cell, summed over the stored days; log scale up to the best
cell), with its own legend section. Each day's grid is a sidecar `adsb/<day>.cov`
written at ingest (or on first request), merged by `GET /api/adsb-coverage`, which also
sums each day per region (Europe, Middle East, North America, South America, Africa,
Oceania, Asia, oceans & other: a cell goes to the first box holding its centre). The
aircraft tab lists those regions like the grids' Gantt: one lane per region, one
segment per stored day coloured against the region's busiest day; hovering a region
outlines and frames its box on the map.

Upload option **Aircraft area → Use map view** passes the visible map as the search
area for recordings without a hum match. The report carries this as `aircraft.flights`
and `aircraft.spectra` (w × h bytes, base64) in `geoloc locate --json`/`--web-json`.

## 8. Results

| Case | Result |
|---|---|
| Synthetic (unit test): two passes 600 s apart in a 20-minute recording, decoys at the same speeds and times 25 km and 40 s off; ±30 min window, 0.8° × 0.6° box | Listener within 150 m, start within 0.5 s, both flights right; also with the start known to ± 3 s and one flyover. |
| The same recording edited: its second half first, then its first half (one flyover per clip), each clip's start known to ± 3 s | Alone, each clip picks the mirror side of its pass (1.8 and 3.6 km off, weak, lead 0%); together **21 m** from the listener, clocks within 0.2 s, both flights right, strong (lead 45%). 56 s on 4 threads. |
| colsto's recording cut up | Not run yet: the recording (YouTube) and the ADS-B bucket were out of this environment's network reach. |
| colsto's recording, 2026-09-08 ADS-B, Long Island box (1.7° × 0.9°), whole day | **40.87621, −73.52335**: 160 m from DOPPLERGEIST's published reference (40.87693, −73.52498, a public map pin, not surveyed); flights N22NY (PC-12) and N93CQ (King Air 350), the reference's; start 20:17:30 UTC (reference 20:17:29.7); moderate, lead 20%, fits 1.04 / 0.92. 59 s on 12 threads. |
| Same, continental US (−125..−66, 24..50), whole day, aircraft alone | The reference place is **#3** (score 101.6) behind two wrong flight pairs at 103.6 and 103.0 (Texas Hill Country; western New Jersey), out of 32,000 places scored; verdict weak (lead 1%). Two flyovers and a whole day leave too many look-alike pairs nationwide: 171k flight segments, 181M corridor cells, 1.4M with one clock, 0.86M silent. 2 h 41 min, 2.2 GB peak. Since pair-first ([§5](#large-areas-pair-first-corridors-and-segment-bounds)): 5.4M corridor cells; on a synthetic rendering of the same two flights, 1 h 06 min → 57 min and 1.04 → 0.67 GB on 4 threads, the rest being centre scoring. |
| Same recording through `geoloc locate` (hum + aircraft), hum searched over 2026-09-08 | The hum alone is **strong** in US Eastern at 20:17:30 UTC; at that second ± 3 s the aircraft search returns the reference place, **strong**, with the same two aircraft. With an area given (Long Island) 73 s; over the whole US Eastern grid (Florida to Labrador, 9,236 flight segments at that hour) 90 s, 1.6 GB, lead 29%. |

What the two runs say: at national scale the aircraft need the hum (or many flyovers,
or a known area). The hum's second removes the time freedom, and with it nearly all
look-alike pairs; the aircraft then turn the hum's continent into a place.

## Limits

- **ADS-B only**: aircraft without ADS-B (or outside ADSB.lol's coverage) cannot be
  matched; the silence test assumes the network saw every low aircraft.
- **A time window is needed**: a day (two or more flyovers) or a few seconds (one).
- **Edited files**: one clock per clip, one place for all; a clip with a single flyover
  needs its own hum second, and the camera is assumed not to have moved between clips.
- **Clutter**: boats, cars and birds are detected too; they are left unmatched.
- **Sound speed, wind**: one effective c; wind shifts the apparent speed by its
  component along the track (covered by the ± 12 m/s margin).
- **Scores are not probabilities**: the verdict rests on fit ratios and the lead over
  the next flight combination.
