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
  (~4 GB tar)     (~0.7 GB/day)     (200 m cells)  (all events   (no unheard   (real trajectory,
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
| **ADSB.lol daily archives** (`adsblol/globe_history_<year>` releases, ODbL) | Free, complete days of every aircraft the network saw, readsb `trace_full` JSON per aircraft. Prod release first, then staging. |
| **Streamed, never stored raw**: tar parts → tar reader → gzip members → worker threads | A day is ~3.9 GB in two parts. 42 s for a whole day on the laptop's 12 threads, ~1 GB peak, constant in the day's size. `--parts` reads local copies. |
| **Jets dropped before parsing**: the type designator is read from the first 512 inflated bytes | Jets are half the traces and most of the bytes. |
| **Missing types from tar1090-db's registry** (`aircraft.csv.gz`, 645k aircraft) | 40% of traces carry no type. Before this, two of the ten best wrong answers on the reference case were business jets (an Embraer Phenom 300, a Citation XLS) of unknown type. Unknown types are still kept (`--known-types` drops them). |
| **Altitude above the ellipsoid**: GNSS altitude where reported; else barometric + that aircraft's median geometric−barometric offset; else + the median offset of all aircraft in its 2° × 2° × 3 h cell | Pressure altitude is off by the day's pressure deviation (hundreds of feet). On 2026-09-08: 34.4M geometric fixes, 0.24M corrected per aircraft, 1.25M per region, 0.02M left raw. |
| **Store**: one file per day, an index (aircraft, type, bounding box, time span) + per-aircraft zstd columnar blobs | A search reads the index and decompresses only aircraft whose box and time overlap. 0.68 GB for 2026-09-08 (35k aircraft, 37M airborne fixes). |
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
| Corridors | Per event, each flight segment flying within the event's speed range (± 12 m/s) in the window is cut in 4 s pieces; each piece writes its closest slant distance into the 200 m cells of its slab (cells whose foot falls in the piece, within reach of the slant range). Per (segment, cell), the minimum; minima at a segment end are not closest approaches. | 3,042 segments → 358k cells, 62 cpu-s |
| One clock | Each (cell, segment) entry implies an interval for the recording's start (CPA reception time − the event's CPA time ± its range ± 4 s). A cell survives if entries for ≥ `min_events` events share an interval. Subsets of the events are tried: clutter stays unmatched. | → 54k cells |
| Silence | Another aircraft passing closer than the matched flyovers' own slant range, between the first and last matched flyover, at a time no flyover was detected, would have been heard: the cell goes. | → 12k cells |
| Exact curves | Each matched segment's curve is modelled for the listener (emission time solved), tabulated against absolute time, and scored against the spectrogram: one start time swept for all events, each event's rest frequency swept on its own (± 2%). A quick brightness fit ranks every survivor, a full fit the best 1,500 at the cell centre, then the best 300 on a 100 m sub-grid; the final fine pass scores on ridges. | 88 cpu-s + 12 s |
| Ranking | Best point per combination of flights; the leading five refined on a 20 m grid. | 11 s |

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

## 7. Results

| Case | Result |
|---|---|
| Synthetic (unit test): two passes 600 s apart in a 20-minute recording, decoys at the same speeds and times 25 km and 40 s off; ±30 min window, 0.8° × 0.6° box | Listener within 150 m, start within 0.5 s, both flights right; also with the start known to ± 3 s and one flyover. |
| colsto's recording, 2026-09-08 ADS-B, Long Island box (1.7° × 0.9°), whole day | **40.87621, −73.52335**: 160 m from DOPPLERGEIST's published reference (40.87693, −73.52498, a public map pin, not surveyed); flights N22NY (PC-12) and N93CQ (King Air 350), the reference's; start 20:17:30 UTC (reference 20:17:29.7); moderate, lead 20%, fits 1.04 / 0.92. 59 s on 12 threads. |

## Limits

- **ADS-B only**: aircraft without ADS-B (or outside ADSB.lol's coverage) cannot be
  matched; the silence test assumes the network saw every low aircraft.
- **A time window is needed**: a day (two or more flyovers) or a few seconds (one).
- **Edited files**: one clock per recording; files with edit points are not searched.
- **Clutter**: boats, cars and birds are detected too; they are left unmatched.
- **Sound speed, wind**: one effective c; wind shifts the apparent speed by its
  component along the track (covered by the ± 12 m/s margin).
- **Scores are not probabilities**: the verdict rests on fit ratios and the lead over
  the next flight combination.
