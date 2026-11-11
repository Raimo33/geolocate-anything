# China's grid frequencies from Raspberry Shake hum (`seismic_cn_*`)

Code: `src/refs/seismic.cpp` (`track_nyquist`, and the fusion shared with
[`seismic_east`](seismic_east.md)), `src/refs/seismic_fetch.cpp` (downloads), station
lists in `src/refs/seismic_cn_*_stations.inc`.

No Chinese operator publishes per-second frequency, and no open measurement campaign
has recorded in China. Mainland China is not one grid but seven synchronous areas tied
to each other by HVDC only, so each needs its own reference. Four of them are
reconstructed here from the 50 Hz hum in citizen-run Raspberry Shake seismographs.

## The grids

Defined in `data/grids.json` (sources cited per area there):

| Grid id | Area | Reference |
|---|---|---|
| `china_north_central` | Beijing, Tianjin, Hebei, Shanxi, Shandong, western Inner Mongolia, Henan, Hubei, Hunan, Jiangxi; Sichuan and Chongqing until mid-2019 | `seismic_cn_north` |
| `china_east` | Shanghai, Jiangsu, Zhejiang, Anhui, Fujian | `seismic_cn_east` |
| `china_southern` | Guangdong, Guangxi, Guizhou, Hainan (from 2009-06-30), Hong Kong, Macau; Yunnan until 2016-07-01 | `seismic_cn_south` |
| `china_southwest` | Sichuan, Chongqing, Tibet, from mid-2019 | `seismic_cn_southwest` |
| `china_northeast` | Liaoning, Jilin, Heilongjiang, eastern Inner Mongolia | none |
| `china_northwest` | Shaanxi, Gansu, Qinghai, Ningxia, Xinjiang | none |
| `yunnan` | Yunnan from 2016-07-01 | none |
| `tibet_central`, `hainan_isolated` | before their ties (2018-11-23, 2009-06-30) | none |
| `taiwan` | Taiwan, 60 Hz | none |

Two dates are approximate: the Sichuan–Chongqing split from Central China (the
Chongqing–Hubei back-to-back stations were commissioned in the first half of 2019; set
to 2019-07-01) and the line through Inner Mongolia (hand-drawn along the Xilingol
League's eastern boundary, good to about 30 km).

The data confirm the outline where stations exist: Beijing and Ordos (600 km apart,
western Inner Mongolia) share one curve, Hong Kong and Shantou share another, and
stations of different grids are uncorrelated (see Validation).

## Why the earlier screening found nothing

- **The open broadband network has almost no hum.** EarthScope holds ten stations inside
  China (network `IC`). Their 40 samples/s channels (1994–2013; 50 Hz folds to 10 Hz)
  show no line on any station (2008-02-15, 2012-02-15: 4–8 dB, 140 mHz of scatter, i.e.
  noise). The 200 samples/s strong-motion channels are archived continuously only at
  Kunming and Xi'an: Xi'an shows nothing, Kunming a 10 dB line in 100 s spectra
  (49.94–50.03 Hz on 2020-02-15), too weak to track by the second. Hong Kong's `HK.HKPS`
  shows a 150 mHz wide bump at the folded frequency, not a line.
- **Everything else records at 100 samples/s**, where 50 Hz is exactly the Nyquist
  frequency. `seismic_ips` discards such channels. Professional digitisers remove the
  line there anyway (`IC` and `HK` 100 samples/s channels: nothing within 1 dB).
- **Raspberry Shakes do not remove it.** Their geophone channel (`EHZ`, 100 samples/s)
  carries the mains line 15–25 dB above its neighbourhood, in homes, schools and offices,
  which is where the hum is. 107 of them are registered in mainland China, Hong Kong and
  Macau. The line just cannot be read the usual way.

## A line on Nyquist

Sampling at 100 samples/s folds 50 + d and 50 − d onto each other. Multiplying the
samples by (−1)^k leaves a real, slow beat

    b(t) = a · cos θ(t),   θ' = 2π d(t)

whose spectrum is symmetric: it gives |d| but not its sign, and with a deviation of
±20 mHz the beat does not even complete a cycle within a 12 s frame. Three properties
recover the signed deviation:

1. **The beat is clean.** Against Continental Europe's measured frequency, 56–100% of
   the power within ±0.5 Hz of the line in eight of nine German Shakes is coherent with
   the grid's true phase over 5-minute blocks.
2. **The amplitude depends on the sign.** The digitiser's decimation filter rolls off
   through 50 Hz, so the line is weaker when the grid runs fast: by 0.73% per mHz
   (−0.72, −0.74, −0.73 on three German samples, r = −0.82 to −0.91 between the log
   amplitude and the true deviation). That is 15% between +10 and −10 mHz.
3. **The phase is smooth.** When d changes sign the beat turns round wherever it is,
   not only at an extremum.

`track_nyquist` decodes the beat with a forward-backward pass over the state
(θ, d): 32 phase bins by 81 deviation bins (±80 mHz in 2 mHz steps), one step per
second, the phase advancing by its row's deviation and the deviation diffusing by one
bin per step. The observation is `a · exp(−κ d) · cos θ` plus noise; `a` is the
station's hum level, re-estimated over 20 minutes (shorter windows let it absorb the
very asymmetry that carries the sign: accuracy collapses below 10 minutes). The
posterior mean of d is the frequency.

Per station the tracker also measures, every 5 minutes, the line's power (within
0.12 Hz) against the noise just beside it (0.14–0.24 Hz), and decodes each two-hour block
a second time without the roll-off (κ = 0). A second is kept only if the line stands
7 dB above the noise, the block's evidence favours the roll-off model by 30 nats per
hour, and the beat is not faster than the decoder's range (see Limits). The second test rejects stations whose line is there but whose sign cannot be
trusted (`AM.R0566` below).

Stations are then fused as in `seismic_east`, with two changes: stations agree from
r ≥ 0.7 (their individual error is 4–7 mHz, not 1), and a station may stand alone only
with a line of 13 dB or more.

## Validation

**Against measured frequency (Germany, Fraunhofer ISE's 1 s series).** One hour from
2025-03-15 04:00 UTC, each station decoded alone, compared after 12 s smoothing (the
grid's own standard deviation that hour: 14 mHz):

| Station | Line | r | RMS error |
|---|---|---|---|
| `AM.R027C` | 16 dB | +0.944 | 4.8 mHz |
| `AM.R142D` | 13 dB | +0.949 | 4.8 mHz |
| `AM.R0B41` | 19 dB | +0.948 | 4.5 mHz |
| `AM.R16CA` | 15 dB | +0.941 | 4.8 mHz |
| `AM.R0076` | 14 dB | +0.932 | 5.1 mHz |
| `AM.R0573` | 17 dB | +0.915 | 5.7 mHz |
| `AM.R0566` | 11 dB | rejected by the roll-off test (decoded anyway: r = +0.19) |
| `AM.R1869` | 6 dB | rejected (line too weak; decoded anyway: r = +0.04) |

The sign is right on every accepted station without being told: it comes from the
roll-off alone. The median of the accepted stations reaches r = 0.964. (This table was
made with the decoder at a 2 Hz step; the shipped setting, 1 Hz with a noise floor of
0.13, scores the same range on these stations.)

**Again with the shipped decoder, on dates it was not tuned on** (six hours from
08:00 UTC each, 12 s smoothing; "fused" is `fuse` with the China settings over the
stations kept):

| Date | Station | Seconds kept | r | RMS error | Sign wrong where \|Δf\| > 10 mHz |
|---|---|---|---|---|---|
| 2024-11-15 | `AM.R0B41` | 59% | +0.915 | 10.8 mHz | 6.2% |
| 2024-11-15 | `AM.R027C` | 62% | +0.908 | 11.1 mHz | 6.2% |
| 2024-11-15 | `AM.R142D` | 32% | +0.917 | 13.3 mHz | 4.6% |
| 2024-11-15 | fused | 65% | +0.914 | 10.7 mHz | 5.3% |
| 2025-08-15 | `AM.R0B41` | 33% | +0.980 | 4.4 mHz | 2.0% |
| 2025-08-15 | `AM.R027C` | 0% (no line that day) | | | |
| 2025-08-15 | `AM.R142D` | 58% | +0.973 | 4.4 mHz | 1.6% |
| 2025-08-15 | fused | 47% | +0.982 | 4.1 mHz | 1.4% |

On 2024-11-15 the error is not spread out: seven of the eight half hours are within
3–6 mHz (r = 0.94–0.99), and one hour (08:30–09:30) is at 18–21 mHz, partly mirrored
(59% of the large errors have the right size and the wrong sign). All three stations
mirror the same stretch, so fusing them does not repair it: the ambiguity is in the
grid's own trajectory (a slow excursion on one side of 50 Hz, where the amplitude
cue has to carry the sign alone), not in any one station's noise.

**Between sites in China** (independent decodes, 1 h, 12 s smoothing):

| Date (04:00 UTC) | Stations | r |
|---|---|---|
| 2024-01-15 | Beijing `R6955` – Ordos `RD3C0` (600 km) | +0.916 |
| 2024-03-15 | Beijing `R6955` – Ordos `RD3C0` | +0.944 |
| 2024-01-15 | Hong Kong `R17EF` – Hong Kong `S6957` | +0.927 |
| 2025-03-15 | Hong Kong `R17EF`, `REB91`, `S6957` (three sites) | +0.84 to +0.92 |
| 2023-03-15 | Hong Kong `REB91` – Shantou `R1025` (300 km) | +0.858 |
| 2024-03-15 | Xuzhou `RD6AB` – Xuzhou `R3EAB` | +0.583 |
| 2024-03-15 | Xuzhou (East) – Beijing, Ordos (North) | −0.03 to −0.08 |
| 2023-03-15 | Chongqing `RED68` (Southwest) – Hong Kong, Shantou | −0.07, −0.07 |
| 2024-01-15 | Chongqing – Hong Kong | +0.03, +0.09 |

**Not yet checked against a Chinese recorder.** There is none in the open. Wuhan
University's ENF-WHU set holds 24-hour mains recordings from the Central China grid,
but without dates; matching them against `seismic_cn_north` would date them and test
the reference at once.

## Coverage

Screening (2026-11-10): 30 minutes from 04:45 UTC on the 15th of every second month,
2017–2026, for all 107 stations. 46 channels returned data at least once; the lists
keep the 21 stations whose line reached 7 dB in at least one probe.

| Source | Stations | When the probes found a usable line |
|---|---|---|
| `seismic_cn_south` | 10 in Hong Kong, 4 in Guangdong | 2019 → today, most probes from 2023; usually 1–3 stations at a time |
| `seismic_cn_north` | Beijing, Tianjin, Ordos | Tianjin 2019-11..2020-03; Beijing 2022-09..2024-05; Ordos 2023-07..2024-03 |
| `seismic_cn_east` | 2 in Xuzhou, 1 in Xiamen | 2023-11..2024-03 |
| `seismic_cn_southwest` | 1 in Chongqing | 2021-03..2024-01 (18 of 19 probes at 10 dB or more) |

The probes sample one half hour in sixty days, so a month between two good probes is
likely but not certain to hold data. Stations hum by spells (a Hong Kong station
qualifies in 7 of 19 probes) and their owners switch them off: after 2024-05 the probes
found no mainland station outside Guangdong.

**Stored** (backfill complete through 2026-11, finished 2026-11-11; share of each
month's seconds stored, total in days of 1 s values):

| Source | Months with data | Coverage | Total |
|---|---|---|---|
| `seismic_cn_south` | 2019-07..2020-09 | 4–27% | 326 days |
| | 2022-01..2022-06 | 14–38% | |
| | 2023-02..2024-01 | 7–90%, 2023-10..12 at 78–90% | |
| | 2024-05, 2024-12, 2025-09..12 | 2–28% | |
| `seismic_cn_north` | 2019-08..2020-03 (Tianjin alone) | 17–74% | 483 days |
| | 2022-09..2023-06 (Beijing alone) | 6–24% | |
| | 2023-07..2024-09 (Beijing and Ordos; Ordos alone from 2024-06) | 80–90% to 2024-04, then 18–56% | |
| `seismic_cn_east` | 2023-10..2024-04 (Xuzhou, Xiamen) | 1–61% | 53 days |
| `seismic_cn_southwest` | 2021-03..2024-02 (Chongqing alone) | 15–78%, most 30–70% | 504 days |

Every other month from each source's first to 2026-11 is recorded as unavailable (no
station online, or no line passing the checks); a few stored months hold almost
nothing (`seismic_cn_south` 2020-10..2021-10 and 2022-07..2023-01 are under 1%).
After 2024 only Guangdong and Hong Kong still give anything, in spells.

North and South decoded independently over June 2023 are uncorrelated (r = 0.03 over
300,594 shared seconds), as two grids must be; both spread about 20 mHz around 50 Hz
and sit a few mHz low on average (−3.6 and −6.8 mHz), which may be a small bias from
the assumed roll-off.

Northeast: 44 stations are registered (38 in Liaoning since 2024-11), but only four
ever returned data and none showed a line. Northwest, Yunnan, Tibet: no station with
data.

One month reconstructed in full through the downloader, `seismic_cn_north` 2024-01:
90.1% of seconds stored, 44% of the month backed by both stations in agreement and 46%
by one alone (Ordos contributed to 87% of the month, Beijing to 47%). It took 0.87 GB
of downloads and 27 minutes on one core, 15 minutes with the chunks decoded on four
threads (same 90.1%: skipping blocks without a line changes nothing that is kept).

## Producing the data

The decoder is the costly part: about 13 s of CPU per station and 6 hours of
recording, so 25 CPU-minutes per station and month (two models per 2-hour block,
forward and backward over 2,592 states every second). The downloads are not
(0.3–0.55 GB per station and month, about 9 s per 6-hour request, parallel requests
are not throttled). So:

- **Never on the dashboard.** The four sources need `GEOLOC_SHAKE_REFRESH`, which the
  dashboard's container does not set: its refreshes skip them, and it only downloads
  the stored months from the bucket. Backfills run on a separate machine and are pushed:

      GEOLOC_SHAKE_REFRESH=1 geoloc refs refresh --grid china_southern   # every month not stored yet
      GEOLOC_SHAKE_REFRESH=1 geoloc refs fetch seismic_cn_north --grid china_north_central --months 2024-01
      geoloc refs push                                                   # with the bucket's GEOLOC_BUCKET / AWS_* set

- **Threads.** The process is single-threaded by design; the trackers are the
  exception. Each downloaded chunk is decoded on a thread of its own
  (`net::run_in_thread`, which wakes the event loop through a pipe, so Asio stays
  built without threads), with `GEOLOC_THREADS` chunks in flight (default: the cores,
  4 to 8). About 110 MB of memory per chunk in flight.
- **Blocks without a line are not decoded.** Where no 5-minute window of a 2-hour
  block reaches 7 dB, its seconds would be dropped anyway; the decode is skipped. A
  station that hums by spells costs only while it hums.
- **Months without a station are cheap**: the archive answers 204 (no data) at once,
  and the month is recorded as unavailable.
- **Grids run side by side**, one process each (a process writes only its own grid's
  manifest). The four China grids are one refresh lane, so the dashboard would run
  them one after another; offline they can run in parallel.

## Limits

- **Accuracy.** A single station is good to 4–7 mHz against 1 mHz for a direct line,
  so short or noisy recordings match less surely here than on other grids.
- **Timing.** Raspberry Shakes keep time by NTP, not GPS: tens of milliseconds, far
  below the 1 s resolution.
- **Station positions** are rounded by the network for privacy (a few hundred metres).
- **One hardware family.** κ was measured on Raspberry Shakes in Germany and is assumed
  the same for every unit. A unit with a different filter would be rejected by the
  roll-off test or come out mirrored; a mirrored station disagrees with its neighbours,
  but a lone one (Chongqing) has nobody to disagree with.
- **Deviations beyond ±80 mHz** are outside the decoder's range. The decoder used to
  answer with values near zero there, at every station alike (found in Western
  Australia against a direct line, [seismic_australia.md](seismic_australia.md)); now
  a beat faster than 60 mHz is detected in the spectrum (64 s windows) and those
  seconds, with half a minute either side, are left empty. Months stored before
  2026-11-10 were decoded without this check; China's large grids seldom leave ±60 mHz.
- **Mirrored stretches.** Where the grid stays on one side of 50 Hz for tens of
  minutes, the amplitude cue carries the sign alone, and now and then a whole stretch
  comes out mirrored (one hour in twelve tested; 2–6% of the seconds beyond ±10 mHz
  have the wrong sign). Every station on the grid mirrors the same stretch, so
  agreement between stations does not catch it. A clip that overlaps such a stretch
  fits it less well, and one lying inside it may not match at all.
- **Coverage follows the hobbyists.** A station hums only while it sits near wiring
  and is switched on; owners move, unplug or replace them. Months between good spells
  are empty, and nothing can fill them afterwards.
- **No live updates.** The dashboard does not refresh these sources; new months appear
  when a backfill is pushed.

## Other routes examined

- *Operators and open repositories*: no per-second series from State Grid, China
  Southern Power Grid, CLP or Taipower on Zenodo, OSF, figshare or the operators' sites.
  KIT's and FNET/GridEye's campaigns have no open recording in China (the GridEye PMU
  release on Zenodo is Cyprus).
- *Several stations as one sensor.* Stations on one grid see the same θ with different
  phase offsets, so two of them form a quadrature pair. Fitting all stations jointly
  (rank 2, loadings tracked through the clocks' drift) reproduces Germany's frequency
  at r = 0.997, 1.4 mHz RMS, with nine stations over 12 hours, but the overall sign
  flips now and then and must be repaired from the amplitude. Not implemented: it
  needs three or more stations at once, which China rarely has. It is the way to a
  1 mHz reference where Shakes are dense.
- *Live radio.* KiwiSDR receivers in China receive mains harmonics directly and are
  GPS-timed; recording them would give a reference from now on, not for the past.

## Beyond China

Nothing here is specific to China. Any 50 Hz grid with humming Raspberry Shakes can be
reconstructed the same way (Western Australia and Darwin are:
[seismic_australia.md](seismic_australia.md); Japan East, IPS/UPS before 2022, India and
South America's 50 Hz grids are candidates); 60 Hz grids fold the line to 40 Hz, which `track_hum`
already reads.
