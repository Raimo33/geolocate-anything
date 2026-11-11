# Compression experiment (reference store)

Question: how small can the stored grid-frequency logs get, and is decoding fast
enough to search compressed data directly?

The experiment script (`scripts/compression_experiment.py`) belonged to the Python
version of this project and is in its git history (before the C++ rewrite); the format
it chose is implemented in `src/store/codec.cpp`.
Every codec is checked to be lossless (decoded output equals stored codes).

## Data

| Grid | Source | Sample | Resolution in the data |
|---|---|---|---|
| Continental Europe | `osf_pgf` (Germany 1 s, France fill) | 2019-01..03 (90 days) | 0.1 mHz |
| Great Britain | `neso` | 2025-01..04 (120 days) | **1 mHz** (NESO logs whole mHz only) |
| Nordic | `fingrid` (10 Hz averaged to 1 s) | 2025-01..04 (120 days) | 0.1 mHz |

The current store spends 16 bits per second. The information actually present per
second (order-0 entropy of the second-to-second change) is much lower:

| Grid | Bits/s needed | Bits/s stored now |
|---|---|---|
| Continental Europe | 2.2 | 16 |
| Great Britain | 3.7 (60-day sample: 4.5) | 16 |
| Nordic | 6.4 | 16 |

## Results (independent 1-day chunks, single thread)

Bytes per second of grid data; "decode" is millions of samples per second.

| Method | CE B/s | GB B/s | Nordic B/s | Decode (Ms/s) |
|---|---|---|---|---|
| **now: raw uint16, mmap** | 2.000 | 2.000 | 2.000 | n/a |
| raw + zstd-3 | 0.549 | 0.886 | 1.402 | 300-420 |
| raw + lzma-6 | 0.411 | 0.692 | 1.064 | 18-56 |
| delta + zstd-3 | 0.401 | 0.625 | 1.052 | 145-170 |
| **delta + zigzag + byte-shuffle + zstd-3** | **0.334** | **0.524** | **0.816** | **~82** |
| delta + zigzag + byte-shuffle + zstd-19 | 0.294 | 0.489 | 0.816 | ~80 |
| 2nd-order delta + zigzag + shuffle + zstd-19 | 0.292 | 0.538 | 0.807 | 58-71 |
| order-0 entropy bound of the delta | 0.278 | 0.468 | 0.805 | n/a |

Per grid-year: **63 MB now → 9–10 MB (CE), 15–17 MB (GB), 26 MB (Nordic)**, a
2.5×–6.8× reduction.

## Findings

1. **Delta + zigzag + byte-shuffle + zstd is within 5–20% of the entropy bound** (Nordic
   within 1.5%). There is little left to gain:
   * Dividing GB deltas by their 1 mHz step adds nothing; zstd already exploits it.
   * Second-order deltas don't help.
   * Linear prediction on the deltas (AR 1–16) gains at most 5% on GB and makes CE and
     Nordic worse. Second-to-second changes are nearly white noise.
2. **zstd-19 vs zstd-3:** 5–12% smaller on CE/GB, nothing on Nordic, same decode
   speed (only encoding is slower, and that happens once per fetched month).
3. **Chunk size barely matters:** 1 h chunks cost about 3–10% vs 1 day, and 1 day vs 30 days
   about 1–3%. One-day chunks give cheap random access.
4. **Decode speed is not a bottleneck.** About 80M samples/s per thread, against a search
   throughput of about 14M lags/s on 4 threads. Decoding a block costs well under 10% of
   correlating it, so the search could read compressed data directly. No
   uncompressed copy or cache is needed.
5. **How much compression helps depends on the source's noise.** Nordic (10 Hz averaged, so the 0.1 mHz
   digits are mostly measurement noise) only reaches 2.5×. Rounding Nordic to 1 mHz,
   as NESO already does, would be lossy. It adds about 0.3 mHz rms against about 1 mHz of
   extraction noise, and would need its own matching-accuracy test before adoption.

## Implemented format

`src/store/` stores one file per grid, source and UTC month, `<YYYY-MM>.u16z`:

```
magic b"GEOZ" | version u8 | 3 reserved | n_chunks u32
n_chunks x (offset u32, length u32)        length 0 = day with no data
day chunks: zstd-19(shuffle(zigzag(diff_u16(codes))))
```

The differences wrap around in 16 bits, so a zigzagged 16-bit delta is lossless
(it matches the 32-bit variant's size at zstd-19). Missing seconds stay code 0.

**Measured on the real store** (21 months: 12 GB, 6 Nordic, 3 CE):

| | Before (sparse `.u16`) | After (`.u16z`) |
|---|---|---|
| Disk | 110.0 MB | 29.3 MB |
| Continental Europe | 63 MB/grid-yr | 9.3 MB/grid-yr |
| Great Britain | 63 MB/grid-yr | 14.2 MB/grid-yr |
| Nordic | 63 MB/grid-yr | 25.9 MB/grid-yr |
| Every stored second after migration | | identical (0 mismatches) |
| Search, 1 GB year, 4 threads | 1.4–2.1 s | 1.5–2.1 s, same match |
| Migration of the old store | | 16.5 s, once |

Encoding costs about 1.3 s per month at zstd-19, paid once at fetch time. Later writes
re-encode only the days they change.

## Lossy option: storing Nordic at a coarser step

The Nordic source is 10 Hz averaged to 1 s, so its 0.1 mHz digits are mostly
measurement noise, and it compresses worst. Does rounding it cost accuracy?

Test: `scripts/eval_rounding.py` (Python version, in git history). 72 synthetic Nordic clips
(2, 5 and 10 min; hum 10, 20 and 25 dB under noise; 8 trials each; recorder clock errors
up to ±50 ppm) were rendered from the **full-resolution** data, then searched against
three copies of the store in which only Nordic was rounded. The other grids are
searched too, so false matches against them count. Every clip runs through every
variant (paired).

| Nordic stored at | MB/grid-yr | Error added to the matched signal | Accuracy 2/5/10 min | STRONG correct | Time error (median / 95%) |
|---|---|---|---|---|---|
| 0.1 mHz (as stored) | 25.9 | 0 | 17% / 96% / 100% | 39/39 | 0.25 / 0.96 s |
| 0.5 mHz | 16.9 | 0.044 mHz rms | 13% / 96% / 100% | 39/39 | 0.25 / 0.97 s |
| 1 mHz | 13.0 | 0.092 mHz rms | 17% / 96% / 100% | 39/39 | 0.26 / 0.96 s |

* The rounding error after frame smoothing and drift removal (what the matcher
  correlates) is under 1% of the 10.5 mHz rms wobble.
* Paired, at 1 mHz: the wobble correlation at the true time changes by +0.0002 on
  average (worst −0.0021), and the score margin by −0.02 ± 0.19, which is noise. No
  clip changed between right and wrong, and the verdict levels were identical.
* The only flip (at 0.5 mHz) was a 2-minute clip with a NONE verdict whose true time
  led by 0.07 points. That is a coin flip, and 1 mHz flipped it back.

**Conclusion:** storing Nordic at 1 mHz (as NESO publishes GB anyway) halves its
storage, 25.9 → 13.0 MB per grid-year, with no measurable loss of accuracy.

**Adopted (2026-11) for every source** as the last step before encoding
(`store.STORE_STEP_CODES = 10`, applied in `write_range`): the full-history store holds
15+ grid-years per grid, and space in the bucket is what limits it. Sources already
published at 1 mHz (NESO, AEMO, Netztransparenz, Energy-Charts) are unchanged by it.
