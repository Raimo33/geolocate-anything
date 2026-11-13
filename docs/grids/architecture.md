# Architecture

How geolocate-anything turns a recording into "grid X, starting at second T", and why
each data structure and algorithm was chosen. Every choice that could be measured was
measured with a benchmark harness (synthetic clips carrying real grid data). Numbers are in
[Measured results](#measured-results).

```
                ┌───────────────┐   ┌──────────────────────────┐   ┌──────────────────────┐
audio/video ──► │ hum detection │──►│ ENF extraction           │──►│ search (every grid,  │──► verdict
  (ffmpeg)      │ 50 vs 60 Hz   │   │ harmonic spectrogram     │   │ every second)        │    + p(false match)
                └───────────────┘   │ → GLRT combine → Viterbi │   │ streaming FFT xcorr  │
                                    └──────────────────────────┘   └──────────▲───────────┘
                                                                              │ decode days
                                  public grid logs ──► normalise ──► compressed month files
                                  (NESO, OSF PGF,     repair, align
                                   seismometers…)
```

---

## 1. Reference store (`src/store/`)

### What the data is

A grid frequency log is a **dense, regularly sampled time series**: one value per
second, for years, per grid. Queries are "give me seconds [t0, t1)", sequentially, in
large blocks. That access pattern decides the structure.

### Structure

```
refs/<grid>/<source>/<YYYY-MM>.u16z   one UTC month: day index + compressed day chunks
refs/<grid>/<source>/manifest.json    provenance, licence, per-month coverage, repairs
```

| Decision | Why |
|---|---|
| **Time-indexed, not rows/tables/B-trees** | The key is the position: second `t` is in the month file named by `t`, day chunk `(t − month_start) // 86400`, at a fixed offset inside it. O(1) addressing, a 16-byte index per day, sequential scans. |
| **`uint16`, `code = round(Δf·10⁴) + 32768`, `0` = missing** | Codes have a 0.1 mHz step over ±3.28 Hz (real excursions stay under ±1 Hz). A reserved code avoids a separate validity mask. The two extreme codes mark values the encoder clipped and are read as missing. |
| **Values rounded to 1 mHz before encoding** (`STORE_STEP_CODES = 10`) | The 0.1 mHz digits of 10 Hz sources averaged to 1 s are mostly measurement noise. Rounding halves Nordic's size with no measurable loss of accuracy ([compression.md](compression.md#lossy-option-storing-nordic-at-a-coarser-step)). Sources published at 1 mHz (NESO, AEMO, Netztransparenz, Energy-Charts) are unchanged. |
| **Compressed: `zstd-19(byte_shuffle(zigzag(diff(codes))))` per day** | The second-to-second change carries 2–7 bits, not 16, and this gets within ~5% of that bound: 63 MB per grid-year becomes 9 (CE), 14 (GB) or 13 MB (Nordic, 26 MB before rounding to 1 mHz). The coding itself is lossless. See [compression.md](compression.md). |
| **One file per UTC month, independent day chunks** | Months are the unit sources deliver. A read decodes only the days it touches (~80M samples/s per thread, under 10% of search time); days with no data take no space. |
| **No derived caches, no uncompressed copy** | The search decodes and filters each block on the fly; it measured as fast as reading uncompressed memory-mapped files. |
| **Sources kept separately, merged at read time by rank** | Provenance is never lost; an official source (NESO) outranks a research archive (the seismometer reconstruction `seismic_east` sits between the OSF archive and KIT's recorders; `seismic_ips` ranks below KIT's, `seismic_ips_shake` below `seismic_ips`), which outranks user recordings. A lower-ranked source is decoded only for month spans where the higher ones left holes, and fills only those seconds. |
| **A download replaces the span its files cover; re-encode changed days only** | From a month's first to its last second with data (spilled hours included), the new download is the truth, gaps too, so a re-fetch after a timestamp fix leaves nothing of the old offset behind. Outside that span the neighbours' spilled hours stay, so months can be fetched in any order; a few spilled hours don't recompress a whole month. |
| **Atomic writes** (write temp + `replace`) for month files and manifests | A crash can't leave a corrupt file; readers see the old or the new version. |
| **Manifest changes under a file lock** (`update_manifest`, `flock` on `manifest.json.lock`) | Several processes can store months of one source at once (a backfill split over units). Each change re-reads the manifest on disk under the lock, applies itself and writes, so no process undoes another's entries. The lock file is not pushed to the bucket. |
| **Optional S3 bucket mirror** (`src/refs/remote.cpp`, `GEOLOC_BUCKET`) | The bucket is the shared copy, the local store the working copy the search reads. Files are compared by MD5 against the ETag. A month file is replaced only by a copy written after it; differing manifests are merged month by month (the entry stored later wins), so one machine's months never drop another's. |
| **Legacy migration** | Uncompressed `<YYYY>.u16` year files from older versions are converted to month files on first use, then deleted. |

### Ingestion

* `RegularAccumulator` streams samples of any rate onto the 1 s grid in **O(month)
  memory**: sums and counts per second. Files are parsed line by line *as they
  download* (or as they inflate out of an archive), so a 700 MB month of 10 Hz CSV is
  never held in memory; a Fingrid month peaks at ~160 MB.
* Finer-than-1 s sources are averaged per second. Coarser ones (France, 10 s) are
  linearly interpolated across gaps up to `max(30 s, 3 × spacing)`; longer gaps stay
  missing.
* Downloads are asynchronous: libcurl's multi interface on one Asio event loop, each
  month a C++20 coroutine (`co_await http.fetch(...)`). Several months stream at once
  with no threads; writes happen on the same loop, one at a time, so the manifest is
  never written concurrently.
* 7z archives (Fingrid from 2022-05) go through libarchive, member by member; ZIPs
  through a small central-directory reader with zlib. Remote ZIPs (Ireland's 2.7 GB
  yearly archives, AEMO's 1.8 GB months) are read with HTTP range requests: only the
  members needed, coalesced into 32 MB requests, two in flight.
* Rate-limited APIs (Energy-Charts) go through one async semaphore and honour
  `Retry-After`, so parallel month downloads queue instead of being refused.
* **When a month is final.** A stored month is not fetched again unless it is
  *partial*. Live API months (Energy-Charts, EirGrid) stay partial until 14 days after
  they end, and each refresh then asks only for the requests or days still under 99%
  full: the hours the API had not published at the month's end, a 404 or an empty day.
  AEMO FPP months stay partial until AEMO's monthly archive replaces the daily files.
  A source whose month index cannot be read (a page redesigned, an API down) fails the
  refresh with the reason instead of passing for up to date; its stored months are still
  refreshed. A nemweb directory that is missing (404) lists nothing, any other failure
  is an error, never "nothing published".
* **US Eastern is reconstructed, not downloaded** (`seismic_east`, `src/refs/seismic*.cpp`,
  `src/refs/mseed.cpp`). GPS-timed seismometers in buildings record the 60 Hz mains
  field, and EarthScope archives them openly. A month fetches the 3 best-scoring stations
  (screened yearly against each other) in 6 h miniSEED requests, plus up to 2 reserves
  where those did not hold up. Each station's hum is tracked per second (one complex FIR
  mixes and low-passes the raw samples, then 12 s Hann frames) and the stations are fused
  by agreement and SNR. EarthScope allows 5 connections: one month at a time with 4,
  429/5xx retried as `Retry-After` says, an account's
  quota used when `GEOLOC_EARTHSCOPE_REFRESH_TOKEN` is set (without it, refreshes leave
  the source alone: the anonymous quota cannot backfill). A month with failed downloads
  is stored as *partial* with its failed chunks listed, and the next fetch downloads only
  those. The full method and its validation are in [seismic.md#us-eastern-seismic_east](seismic.md#us-eastern-seismic_east).
* **US Western and ERCOT likewise** (`seismic_west`, `seismic_ercot`): the same code and
  account. Western covers 2001-09..2018-12 and 2025-08 on, leaving AESO's months out;
  ERCOT covers 2014-02 on. Both rank below the measured recorders and store a second
  only where two sites agree (ERCOT from 2015-02)
  ([seismic.md#us-western-seismic_west](seismic.md#us-western-seismic_west), [seismic.md#ercot-seismic_ercot](seismic.md#ercot-seismic_ercot)).
* **IPS/UPS likewise, from 2022-10** (`seismic_ips`): the same code at 50 Hz over nine
  strong-motion stations in Azerbaijan, Kazakhstan and Kyrgyzstan, 4 per month plus up
  to 2 reserves, and no station accepted alone. It shares the EarthScope account with
  `seismic_east`, so the two grids refresh one after the other
  ([seismic.md#ipsups-from-azerbaijan-and-central-asia-seismic_ips](seismic.md#ipsups-from-azerbaijan-and-central-asia-seismic_ips)).
* **Brazil, 2004-02..2008-08** (`seismic_brazil`): the same code on two 100 samples/s
  channels of one recorder in the University of São Paulo's own FDSN archive (no
  account). A fixed tone beside the hum sits among the noise probes, so this network
  takes the median of the probes as its noise floor; one channel may stand alone
  ([seismic.md#brazil-seismic_brazil](seismic.md#brazil-seismic_brazil)).

### Data repairs found by cross-checking (all automatic, recorded in the manifest)

| Issue | How it was found | Fix |
|---|---|---|
| OSF GB values are mHz but have median ≈ 53, so a median-based unit guess called them Hz | Compared against NESO's official file for the same month: r = 1.000 at lag 0 but a 50 Hz offset | Unit detection by *range* (absolute values sit within 40–70 Hz; mHz deviations are signed and span > 2) |
| France files: days 1–12 have **day and month swapped** (2 Jan stored as 1 Feb) | France coverage was 64.5% even though every row was present | `repair_day_month_swap`: in a month-M file, a row in month ≠ M whose day = M is moved back. 95,040 rows per month repaired |
| France timestamps are **17 s early** relative to Germany (same grid) | Cross-correlation of the two series: peak at −17 s, identical in every week and every month (r 0.993–0.997) | Secondary sources are aligned to the primary by FFT cross-correlation on their overlap before filling its gaps. The measured default is used when there is no overlap |
| NESO historic months are sometimes `.zip` with irregular names | Index had holes | Index by URL pattern *or* resource title; transparent unzip |
| Finland (Fingrid, and the OSF copy) is in **Finnish local time**, not UTC: the Nordic reference was 2 h (winter) / 3 h (summer) late | Fingrid documents UTC+2/+3; its 2025-10-26 file has 899,933 rows (25 h at 10 Hz) with 03:xx twice; the OSF Finland month equals Fingrid's at zero shift (r = 1.0000) | `local_to_utc_seconds`: zone conversion, with the repeated autumn hour assigned by order (running maximum of wall time, carried across chunks). Rows outside the UTC month are *spilled* into the neighbour (part of the span the download replaces). Stored months carry a `format` number; older Nordic months count as stale and are fetched again |
| OSF Germany and France are **German local time**, not UTC: CE 2011–2020 was 1–2 h early | OSF Germany equals the Netztransparenz series (r = 1.000) offset by exactly 7200 s in summer and 3600 s in winter, flipping on the DST-change day | Converted from Europe/Berlin, then Germany's 2 s logging delay removed (France's default alignment becomes 15 s). CE months carry format 2 |
| Netztransparenz runs **2 s late** | Peak at −2 s against KIT's GPS-timed Karlsruhe recorder (2019-04, r = 0.997) and Energy-Charts (2022-05) | Shifted −2 s after the zone conversion. After both fixes: Energy-Charts vs Netztransparenz lag 0 (r = 0.9987), Netztransparenz vs OSF lag 0 (r ≥ 0.99995 in 2014, 2018 and 2020) |
| Open-ENF's DE database runs **2 s late** from 2011 (the German TSOs' series, uncorrected) and **about 5 s late** in 2010 (another recorder, 10 mHz steps) | 2011-07/2011-12 against OSF Germany and 2022-02 against Netztransparenz: peak at +2 s (r = 0.9996, identical in 2022). 2010 has no overlapping source: the hourly schedule-change steps sit 3 s later than in its 2012 data (quarterly +4/+3/+2/+3 s, controls 0..−1 s); the daily pattern rules out an hour error | Read from stamp + 5 s (2010) or + 2 s (2011-02..06); 2010 is good to about ±2 s |
| AEMO's FPP regions disagree: **SA1 and VIC1 run 5–6 s behind NSW1** (QLD1 within a second), so their four-region mean ran 2 s late | Cross-correlating the regions' 4 s series (2025-01, 2026-09, every 10-day window); the mean sat 2–3 s behind the NSW-sampled yearly workbooks, Tasmania (one region) at lag 0 | Mainland = NSW1, gaps from QLD1; FPP months carry format 2 |
| AEMO FPP months **lost their last 10 h**: NEM-time files, no spill | The 2026-09 monthly archive ends at 2026-10-01 00:00 NEM (2026-09-30 14:00 UTC); older archives happened to hold the next month's first day | The month's files are read with a day's margin and the overflow spilled into the neighbours, as for Causer Pays |
| NESO files before mid-2016 are **UK local time** with day-first dates | The May 2014 file runs from 01:00 on 1 May to 00:59:59 on 1 June | Converted from Europe/London; NESO months carry format 2 |
| EirGrid's dashboard is **Irish wall-clock time** and **2 s late** | On the spring-change day the local 01:00 hour is all null; peak at −2 s against KIT's GPS-timed Irish recorder (2021-12, r = 0.997) | Converted from Europe/Dublin, then −2 s |
| AESO's "Timestamp (UTC)" runs **3 s late** | Peak at −3 s against KIT's GPS-timed Colorado recorder (2024-04, r = 0.993) | Shifted −3 s |
| NESO's **2025-04..2025-10 files are UTC+1** (BST) all month; everything else from 2020 is plain UTC. (An earlier reading blamed the London PMU instead, as "UTC minus the UK offset".) | London's PMU (Zenodo 21461024) matches NESO 3600 s later from 2025-04-28 to 2025-10-31, at lag 0 from 1 November; London's stamps have no jump or repeat there, nor at either clock change, and no repeated content (its Oct 31 23:00 and Nov 1 00:00 hours: r = 0.05). That hour of London matches no NESO hour, and the 2025-03 file's last hour equals the 2025-04 file's first (r = 1.000); no other year's seam does | NESO's 2025-04..10 plain times −3600 s (format 3 for those months); London read as UTC (format 2) |
| KIT's 2025 Baltic recorders (0.1 s) run **1.3 s early** | Berlin vs Energy-Charts, 2025-02-10..11, with the 0.1 s rows shifted in 0.2 s steps before averaging to 1 s: r peaks at +1.3 s (0.9976, 0.9958 unshifted); Kaunas after joining CE matches Berlin at lag 0 (mean difference 0.00 mHz) | Shifted +1 s (whole seconds only) |
| Grid-India's PMU export is **IST** | Frequency steps from the hourly schedule changes sit at minute 0 of the stamped hour | Converted from UTC+5:30 |

NESO's 2015–2019 files carry an explicit UTC offset in every row; from 2020 the times are
plain UTC, except the BST-stamped 2025-04..10 files above. Energy-Charts returns unix
seconds (UTC by definition). The KIT EDR recorders and the Irish FDR are GPS-disciplined
and log UTC (the Irish DST-change days hold 24 full hours). AEMO data is NEM time
(UTC+10 all year).

The Nordic results under *Measured results* below are unaffected by the Finland fix.
Test clips are synthesised from the stored reference itself, so a constant time shift
cancels out. Real recordings, however, were matched 2–3 h off on Nordic before the fix.

### No derived caches

Matching needs the reference *smoothed like one extractor frame and with drift
removed*. Each search block is read raw and filtered on the fly (gap bridging, frame
smoothing, moving-average high-pass: all O(n)), which is cheap next to the FFT
correlation. Nothing but the raw data is stored, so disk use is exactly the reference
data and there is no cache to invalidate. (Earlier versions kept two prepared copies
per grid under `_derived/`, tripling disk use; delete that folder if you have one.)

---

## 2. ENF extraction (`src/enf/extract.cpp`)

### Hum detection

Segment-averaged PSD (16 s Hann segments, 50% overlap), but each segment's band
around a harmonic is first divided by that segment's own noise floor there (median of
the ring below). A plain Welch average lets loud passages (speech, music, splashing)
set the floor for the whole file and buries hum that is steady but only audible in the
quieter parts: a 27-minute YouTube video measured 7 dB at 100 Hz that way and 18 dB
whitened. Silent segments (60 dB under the typical floor) are skipped. Each harmonic's
peak is compared with the median of a surrounding ring. Harmonics shared by both families (300, 600 Hz) are ignored when deciding 50 vs
60. Hum is declared present only if the best family's strongest line is ≥ 10 dB. White
noise alone often produces a 6 dB bump somewhere among dozens of bins; a test covers
this.

Harmonics above 0.45 × the file's *original* sample rate are ignored: audio resampled
up from a low rate has no real content there, only fake harmonics.

**Faint hum: ridge test.** When no line reaches 10 dB, hum can still be present as a
steady track that no single long-term spectrum shows. Each family's harmonics are then
tracked (12 s frames, 5 mHz steps, Viterbi as below) and the best track evidence of the
1–4 strongest harmonics combined is compared with the same score at ~24 "null"
fundamentals between 40 and 60.5 Hz whose harmonics stay clear of every 50 and 60 Hz
line. A Gumbel fit to the nulls gives a p-value; the family is accepted at p ≤ 10⁻³
(clips of at least 45 s).

When both families carry harmonics (often a device's own 120/240 Hz series next to a
50 Hz hum), the other family is extracted too if it has two or more harmonics ≥ 3 dB,
and the grid data decides.

### Edit points (`src/cuts.cpp`)

An edited video joins clips recorded at different moments, so no single time fits the
whole trace. `locate` therefore first looks for cuts and analyses each continuous
segment on its own (hum detection, extraction, search, verdict).

- **Cue: the hum's phase.** Within one recording the phase advances smoothly at the
  grid frequency; a cut joins two unrelated phases (Rodríguez, Apolinário & Biscainho,
  IEEE TIFS 2010; several harmonics as in their EUSIPCO 2013 follow-up). The band
  around each of the 3 strongest harmonics is read once per second as a complex
  amplitude. At every boundary, each side's frequency is the circular mean of 15 s of
  phase steps, and the phases of 5 s on each side, rotated to the boundary, are
  compared. The difference is divided by its local robust spread (MAD over ±60 s), so
  where the hum is buried the differences spread over ±π and nothing scores high.
- **Why two-sided averages, not one-window spikes.** One faded window's phase can land
  anywhere; a spike test called those cuts. A real cut's offset persists, an outlier
  doesn't.
- **Placement.** The window test spreads a cut over 5 s. A change-point fit (constant
  complex amplitude on each side, ±10 s) then places it to a band sample (~0.4 s).
  Comparing plain sums instead slides towards the louder clip.
- **Thresholds:** z ≥ 7, or z ≥ 5 with a ≥ 6 dB jump of the background-noise floor
  (Pan, Zhang & Lyu, ICASSP 2012). Noise level alone is not enough: speech and music
  move it constantly.
- **Not used:** the frequency step across a cut (Esquef et al., IEEE TIFS 2014). It found
  ~5% more cuts but brought back false cuts on unedited audio.
- Segments under 60 s are reported but not searched. When several segments are searched,
  the best one's false-match chance is corrected to 1 − (1 − p)ⁿ before its level sets
  the file's verdict.

Synthetic splices (real Continental Europe wobble, 8 clips of 15–120 s with random
phases and ±10 dB level changes; 20 files per row), and 20 unedited 10-minute files:

| Broadband SNR | Cuts found (within 2 s) | False cuts, edited files | False cuts, unedited files |
|---|---|---|---|
| −10 dB | 78 / 140 (56%) | 2 | **0** |
| −20 dB | 37 / 140 (26%) | 5 | **0** |
| −25 dB | 1 / 140 | 0 | 0 |

The misses are mostly cuts whose random phase step happens to be small. Unedited
files must stay in one piece, so the thresholds favour that. Not yet measured on real
unedited recordings.

### Choosing harmonics by agreement

Device tones (chargers, fans, coil whine) near mains harmonics are often louder than
the hum, so loudness is a poor guide. Every harmonic of the family above 3 dB (up to 8)
is tracked on its own (steps 1–5 below, one harmonic each). The tracks are smoothed
like one frame and high-passed (61 frames), since unrelated slow drifts correlate by
chance. The largest group whose tracks agree pairwise (r ≥ 0.5) is used; ties go to the
group nearer the nominal, then the higher mean r. Harmonics among the 6 strongest that
missed the group rejoin it if they follow its mean track (r ≥ 0.25), which keeps
evidence in heavy noise. The runner-up group is searched as a second hypothesis: a
phone's own harmonic series can agree as well as the mains does, and only grid data
separates them. With no agreeing pair, the 6 strongest harmonics above 6 dB are used.

### Tracking: spectrum combining + Viterbi

For each chosen harmonic *h*:

1. **Heterodyne via one FFT.** Take the rfft of the whole recording once, cut the bins
   around `h·f₀`, and inverse-FFT them into a complex baseband signal at a few Hz.
2. **Exact DFT on a shared grid.** Frames (Hann, 12 s by default, 1 s hop) are evaluated *exactly*
   at the frequencies `h·(f₀ + δ_k)` for a shared grid of fundamental offsets
   `δ_k ∈ [−0.25, 0.25] Hz`, 1 mHz apart. This is one complex matrix product
   (frames × samples) · (samples × offsets). There is no zero-padding and no
   interpolation error.
3. **Noise normalisation.** Each frame is divided by its own noise floor: the median
   power at 48 frequencies just outside the band, divided by ln 2 (the median of an
   exponential distribution is mean · ln 2). Pure noise then averages 1.

Then:

4. **GLRT combination.** For a tone with unknown amplitude and phase in Gaussian noise,
   the log-likelihood ratio at frequency δ is the noise-normalised power minus 1. Summing
   over harmonics is the joint GLRT. Weak harmonics add evidence instead of adding
   outliers, which is what independent peak picking + fusion (the baseline) did.
5. **Viterbi ridge.** The grid frequency is physically continuous, so we find the path
   maximising `Σ_t ε·LLR(t, k_t) − Σ_t (Δk_t · 1 mHz)² / (2σ²)`. That is the GLRT
   evidence plus a Gaussian random-walk prior. Cost: O(T · K · J) time,
   O(T · K) `int16` back-pointers.
   * Because scaling the objective doesn't move its optimum, ε and σ collapse into one
     parameter, κ = ε·σ². This was confirmed empirically: configurations with equal
     κ gave identical results.
   * κ = 4 mHz² (σ = 2 mHz per 1 s hop, ε = 1) was chosen on real data.
   * Steps larger than 20 mHz between frames are not allowed at all.
   * The prior is *automatically SNR-adaptive*: strong frames have sharp, tall LLR
     peaks that dominate, while weak frames lean on the prior and bridge dropouts.
6. **Sub-grid refinement.** A parabola on the log combined power around the path.

---

## 3. Search (`src/enf/match.cpp`)

### The problem

Score every alignment of an m-sample query against N seconds of reference, for every
grid. N is tens of millions per grid-year.

### Why exact sliding correlation, not fingerprints

For a known waveform in noise, the matched filter (sliding correlation) is the optimal
detector. Hash or landmark indexes (à la audio fingerprinting) are sublinear, but they
quantise away exactly the weak, noisy evidence that low-SNR recordings consist of. So
the search stays exact and is made fast instead. Fingerprinting's other idea, a cheap
pass that proposes candidates and an exact one that verifies them, does carry over (the
two-pass search below).

### Score

The base score is Pearson r of the **drift-removed** signals:
* the reference is smoothed like one extractor frame;
* a centred moving average (window 61 s, or the largest of 15/31/61/121 that fits the
  clip) is subtracted from both.

Slow drift looks alike on every grid and every day, so it inflates chance matches. The
fast wobble is what is unique. Pearson also ignores the recorder's clock error, which
shows up as a constant offset (reported as `offset_hz`).

### Kernels

| Case | Method | Cost per block of B lags |
|---|---|---|
| Gap-free reference window (common) | **Overlap-save** FFT cross-correlation q ⋆ y in **single precision**, with FFTs of N ≈ 4–8 m (4096 for a 10-minute clip: cache-resident, FFTW-planned), each yielding N − m + 1 lags. Window mean and energy of y from chunk-local prefix sums (double) | 2 FFTs of size N per N − m + 1 lags, O(1) per lag rest |
| … query with missing frames (most real recordings) | The same, with the query centred over its valid frames and zero elsewhere, so q ⋆ y is the covariance term exactly; the window's sums over the valid frames are one prefix-sum difference per valid run of the query | as above, plus O(runs) per lag |
| Reference gaps | Lags whose window touches a gap are split into runs. Only those runs (widened by 4m to avoid tiny FFTs) use the exact NaN-aware masked formula (six correlations in double; three when the chunk itself is gap-free) | proportional to gaps × m |

* Reference gaps of ≤ 10 s are bridged linearly before smoothing. Generator inertia
  makes faster change physically impossible. This took Nordic from 290 to 34 gap runs
  a month, and its throughput from 4.9 to 17M lags/s.
* All moving filters are O(n): tiled prefix sums, and the 12-tap frame smoothing as a
  vectorised FIR that renormalises only next to gaps.
* Peak picking is O(B): a lag that is the maximum over ±m/2 is also the maximum of its
  own block of m/2 + 1 lags, so only block maxima are candidates, each confirmed
  against its two neighbouring blocks; then a partial sort.
* **Joint score.** Pearson on the wobble ignores the absolute level, for example
  "the clip sat 15 mHz below 50 Hz". A recorder's clock error moves that level by
  only ±1–2.5 mHz, but the grid's level wanders with σ ≈ 22 mHz in Continental
  Europe and 67 mHz in GB, so the level is strong evidence. Each lag gets a
  log-likelihood ratio with two parts:
  * **shape:** `sign(z)·z²/2`. Here `z` is Fisher-z of r, standardised by the robust
    spread of all lags in the block, so the effective sample count calibrates itself.
  * **level:** `log p_clock(offset) − log p_grid(offset)`. The clock prior is a
    mixture: 80% σ = 2 mHz, 20% σ = 10 mHz. This depends on the offset alone, so it
    is tabulated per block (20,001 entries) and applied with one gather.

  On identical scenarios it found the same or more right answers, with false-match
  probabilities 3 to 9 orders of magnitude lower.
* **One pass per grid for the whole file.** Every segment and hum hypothesis of a file
  is extracted first; each block of each grid is then read, decoded and filtered once
  and correlated against all of those queries. A 30-minute livestream split into
  5 segments costs one scan per grid, not ten.
* **Two-pass search** (borrowed from audio fingerprinting's "cheap candidates, then
  verify", without its hashing). The wobble is band-limited: 12 s frames above, the
  61 s high-pass below. So block means of 4 samples keep almost all of it, and a first
  pass correlates and scores the 4x-decimated signals (4x fewer lags, shorter FFTs).
  It keeps the best 512 *bumps* (local maxima one frame apart) per block. The second
  pass computes the exact r and joint score at the lags around each one, normalised
  from the exact r at the same 20,000 sampled lags as the single pass, then keeps those
  that beat everything within ±m/2. Scores are therefore the single pass's own; only
  which lags get scored depends on the first pass. Shortlisting regional maxima
  instead of bumps was not enough: which bump of a region is highest can differ
  between the decimated and the exact score. Clips under 128 s use a smaller factor
  (60 s: single pass), and blocks under 2^18 lags stay single-pass.
* **Blocks on every core.** The reference is cut into 2^20-lag blocks; each is read,
  decoded, filtered, correlated and shortlisted on its own in O(B) memory (~30 MB per
  thread), whatever N is. Threads take the next unstarted block (`GEOLOC_THREADS`,
  default every core), and the blocks' candidates are merged in time order, so the
  result is the same to the bit on any number of threads.
* Until 2026-11 a query with any missing frame (87% valid is typical of a real
  recording) took the double-precision masked kernel at every lag and a scalar exact
  r, and reference smoothing renormalised every output inside a long gap: 6.5M lags/s
  per core. With those fixed and the SIMD rescoring, a 10-minute clip against all 14
  stored 50 Hz grids (1.4 × 10⁹ lags) is searched at ~19M lags/s per core: 437 s
  became 167 s on one core and 39 s on 8 threads (laptop i5-1235U), with the same
  top-10 candidates on every grid.
* The best peaks get sub-second refinement (parabola through r at lags i−1, i, i+1).

### Verdict: how sure are we?

Almost every lag of every grid is a *wrong* alignment. The top wrong scores form the
upper tail of an extreme-value (Gumbel) sample, whose consecutive gaps are
exponential with mean β/i.
* β is estimated from the 2nd…30th best peaks.
* If the winner were also wrong, its lead over the 2nd best would be Exp(β), so
  `p_false = exp(−lead / β)`.
* This self-calibrates for clip length, search-window size and grid.
* Levels also need a minimum wobble correlation r of the winner:

  | Level | p_false | r |
  |---|---|---|
  | STRONG | < 10⁻⁴ | ≥ 0.6 |
  | MODERATE | < 10⁻² | ≥ 0.5 |
  | WEAK | < 0.1 | ≥ 0.5 |
  | NONE | otherwise | |

  The r floors guard against a poor-shape match whose lead only *looks* rare: pooling
  more grids shrinks the tail scale β with unchanged scores (a YouTube clip at r = 0.40
  became a wrong WEAK when references were added).
  With too few peaks to estimate β (under 6), only r ≥ 0.8 gives WEAK.
* All hum hypotheses of a segment (agreeing group, runner-up group, other family) are
  merged per grid before this, so the estimate counts every hypothesis's wrong matches.

---

## Measured results

Store: Continental Europe, Great Britain and Nordic, 2019-01 to 2019-06 (plus GB
2026-07 to 09), real data. Scenarios: random real moments, random clock error
(±50 ppm), white noise at the given broadband SNR, 1 kHz analysis rate.

### Extraction (wobble correlation with truth, 300 s clips, 8 s frames)

| SNR | Baseline (per-harmonic peaks + fusion) | GLRT + Viterbi |
|---|---|---|
| −10 dB | 0.965 | 0.964 |
| −20 dB | 0.804 | **0.939** |
| −25 dB | 0.181 | **0.829** |

### Identification (correct grid and start within 2 s; 9 trials per cell)

Each query searches **all** stored data of all three 50 Hz grids (about 55M
candidate alignments).

| Clip | Pearson only, 8 s frames | **Joint score, 12 s frames (default)** |
|---|---|---|
| 300 s, −25 dB | 44% | **100%** |
| 300 s, −20 dB | 78% | **100%** |
| 300 s, −10 dB | 100% | **100%** |
| 600 s, −25 dB | 89% | **100%** |
| 600 s, all SNRs | 89–100% | **100%** |
| 120 s, −10 dB | 56% | 44% |
| 120 s, ≤ −20 dB | 0% | 0–11% |

* **Frame length** was chosen by this benchmark:
  * 4 s and 6 s frames were noisier and scored 0% at 300 s, −25 dB.
  * 8 s scored 67%.
  * 12 s, 16 s and 20 s all scored 100% on 300 s and 600 s clips, and 12 s was best
    on 120 s clips.
* **120 s clips are an information limit, not a software one.** Two minutes of
  frame-smoothed wobble hold roughly ten independent samples, which is not enough to
  be unique among tens of millions of candidate seconds. Narrow the search with
  `--from/--to` when you have side information.
* **Timing precision when correct:** median 0.19 s, 95th percentile 0.80 s, thanks to
  sub-second peak refinement.

### Calibration: is the confidence honest?

| Verdict | Correct (default config) | Correct (all 7 configurations tested) |
|---|---|---|
| STRONG | 41 / 41 | **221 / 221** |
| MODERATE | 8 / 8 | 61 / 63 |
| WEAK | 5 / 7 | mixed (it is a hint, not a finding) |

A STRONG verdict was never wrong in any configuration.
