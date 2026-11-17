# Aircraft history store

Question: how small can a day of ADS-B history get while the search still reads every
flight as accurately as the raw data allows? Implemented in `src/air/adsb.cpp`.

## From fixes to trajectory nodes

A day of ADSB.lol history is ~4 GB of raw position fixes (readsb `trace_full` JSON per
aircraft). It is streamed, never stored raw, and jets are dropped before parsing (half the
traces, most of the bytes). The search reads a flight as a cubic Hermite curve through
positions and velocities, so instead of every fix the store keeps only that curve's
*nodes*:

- Spikes (bad decodes, barometric glitches) are dropped, and each fix is smoothed
  (quadratic least squares over ±6 s, positions and reported velocities).
- From each node, the next is the farthest fix whose curve stays within 25 m
  horizontally, 15 m vertically and 5/3 m/s of every smoothed fix between, at most 100 s
  apart; leg starts and gaps always split. In practice: a node per turn, climb or speed
  change, one per 100 s on a steady leg.

## Coding

Nodes sit on whole seconds. Velocity (0.5 m/s steps) and position (5e-5°, ~4 m) are coded
as integer residuals from the previous nodes (velocity extrapolated, position moved by the
trapezoid of the velocities), written in 16-bit byte planes and compressed with zstd
level 19: **5.7 bytes a node**. One file per day holds per-aircraft blobs and an index
(aircraft, type, bounding box, time span) at the end, so a search decompresses only the
aircraft whose box and time overlap.

## Results

| Day | Fixes | Nodes | Store |
|---|---|---|---|
| 2026-09-08 | 36.9M | 8.5M | **48 MB** |
| 2026-11-12 | 33.0M | — | 41 MB |

- **~4 GB → ~48 MB a day, about 80× smaller.** Earlier formats: v1 0.68 GB, v2 ~0.22 GB.
- **Accuracy:** against the raw fixes the curve is within 11 m (median) / 31 m (95%) and
  0.7 / 5 m/s in radial velocity, which is the raw fixes' own disagreement between
  reported velocity and position.
- **Whole archive:** all of ADSB.lol's history (~1,350 days) fits in ~65 GB.
- **Tried and rejected:** coarser tolerances gain little (50 m: −20%); larger zstd frames
  5%; velocity derived from node positions instead of stored: worse.

## Chunk index

**Chunk index** (sidecar `adsb/<day>.idx`, not a store version): per aircraft, its nodes in 10-minute slots, each slot's box (0.01°, rounded outwards) and node speed range (1 m/s); columnar, zstd 19.

`load_tracks` decompresses an aircraft only if a slot within 120 s of the window has a node in the box (the segments it would yield need two such nodes 120 s apart at most, so the result is the same), and, given a speed range, a node at that speed. Built from the store alone: at ingest, on first use when missing or stale (it records the store's size and tail), or with `geoloc aircraft index [DAY...]`, so the bucket's ~1,350 v3 days need no rebuild and no new download (3 s a day; 1,350 days ≈ 70 min on 4 threads, or lazily as days are used). 2026-09-08: 35,129 aircraft, 448,527 slots, **2.8 MB (5.8% of the store)**. Not uploaded: a server rebuilds it the first time a day is used. Aircraft decompressed, before → after: the US in 3° regions over a whole day 84,265 → 61,429 (73%); the US Eastern extent around the hum's second 6,733 → 5,252 (78%); Long Island then 494 → 278 (56%). With the event speeds (72–178 m/s) as well, about 42% of the region loads remain, but the search does not filter by speed when loading: the silence stage needs aircraft of every speed (it skips the others itself, by their segment bounds). Loading is a few seconds of a large search either way.
