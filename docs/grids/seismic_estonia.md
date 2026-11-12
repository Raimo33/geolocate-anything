# IPS/UPS grid frequency from Estonian seismometers (`seismic_estonia`)

Code: the same as [`seismic_ips`](seismic_ips.md) (`src/refs/seismic.cpp`,
`src/refs/seismic_fetch.cpp`, network `estonia()`); the screened stations are in
`src/refs/seismic_estonia_stations.inc`. Data: GEOFON's FDSN dataselect, network `EE`
(Estonian National Seismic Network, Geological Survey of Estonia,
doi:10.14470/RR425351). No account needed.

Estonia was part of IPS/UPS until the Baltic states left it on 2025-02-08 at 07:08 UTC.
The Estonian national network has recorded at 250 samples/s with GPS time since 2015-05,
so the 50 Hz line sits well below Nyquist and is read directly, as in `seismic_ips`. That
takes the IPS/UPS reference back from 2022-10 to 2015-05. The network records on after
2025-02-08, but then Estonia is in Continental Europe, which has measured references:
no month after 2025-02 is listed and nothing from 07:08 UTC on 2025-02-08 is stored
(`Network::until`).

## Screening (2026-11-12)

Candidates: every vertical channel of network `EE` at 250 samples/s (EE01–EE08 `HHZ`
from 2015–2017; the stations renamed in 2023, `NOPE`, `TOSE`, `PISE`, `SRGE`, and `PLDE`
from 2024, `CHZ`). The older `ARBE`, `SRPE` and `TRTE` (80 samples/s, 1996–2017) show no
line, folded or not.

Probes: 30 minutes from 01:45 and 11:45 UTC on the 5th, 15th and 25th of every month,
2015-05..2025-02 (708 windows), tracked with `track_hum`. A station's score for a year
is the share of its probes in which it agreed with another site (more than a kilometre
away) at r ≥ 0.9 after 60 s smoothing.

Share of probe windows in which at least one pair agreed:

| Year | 01:45 UTC | 11:45 UTC |
|---|---|---|
| 2015 (from May) | 26% | 17% |
| 2016 | 61% | 28% |
| 2017 | 31% | 11% |
| 2018 | 89% | 58% |
| 2019 | 47% | 17% |
| 2020 | 72% | 47% |
| 2021 | 81% | 53% |
| 2022 | 97% | 67% |
| 2023 | 64% | 39% |
| 2024 | 50% | 0% |
| 2025 (Jan–Feb) | 83% | 0% |

The hum is mostly a night-time signal. Most agreement comes from `EE01` (59.21° N 23.62° E)
and `EE08` (58.66° N 25.24° E), 110 km apart. They agree in 50–80% of probes in 2018–2023, at
r = 0.99–1.000 when both hum. `EE02` and `EE05` carry 2015–2016; `PISE` and `SRGE` carry
2023–2025. Stations within a kilometre of each other count as one site, so a renamed
station is never its own predecessor's partner.

## Validation

- *Against KIT's recorders.*
  - St. Petersburg (`osf_edr` RU01, 2019-04-30..05-11, 600 km east): r 0.96–0.997 in 6 h
    windows, with the KIT series 1 s behind throughout (sub-second peak +0.8 s).
  - Kaunas and Tallinn (`osf_baltic`, 2025-01-23..02-08, KIT's 0.1 s recorders, already
    corrected against Continental Europe): r 0.95–0.996 and 2.7 mHz RMS after 12 s
    smoothing over 432,000 shared seconds, with the seismic series 0.9 s ahead.

  So the reconstruction is about 1 s ahead of KIT's time base. Where the second lies
  (the 12 s frame convention here, KIT's 1 s averaging there) is not settled. The other
  seismic sources share this code: `seismic_ips`'s synthetic test clip came out 1 s
  early too.
- *Between sites:* r 0.94–1.000 between EE01, EE06 and EE08 (60–130 km apart), lag
  0–1 s.

## KIT's 2019 Tallinn recorder

KIT's 2019 file for Tallinn (`osf_edr` EE01, OSF `t5ske`, 2019-03-25..04-17) has a
second column from Karlsruhe on the same time axis. That column follows Continental
Europe's measured series (`netztransparenz`) at lag 0 every day, so the time axis itself
is right. Tallinn's own column is not: against this reconstruction it runs late by an
offset that grows by 1.2–2.3 s a day.

| Time (UTC) | Tallinn late by |
|---|---|
| 2019-03-29 23:00 | 20 s |
| 2019-04-03 19:30 | 26 s |
| 2019-04-06 20:30 | 33 s |
| 2019-04-11 22:30 | 43 s |
| 2019-04-15 03:00 | 51 s |
| 2019-04-17 00:30 | 56 s |

The offsets come from 1–6 h windows at r 0.97–0.99. Before 03-29 there are too few humming
seconds. A 6 h window on 03-26 peaks at 33 s, but the peak is broad (r 0.88 at 20 s), and
if it were real it would mean a reset. Those four days are therefore dropped, not guessed.

`edr_clock()` (`src/refs/sources_archives.cpp`) moves each Tallinn row back by the
offset at its stamp, less 1 s, which puts Tallinn on the time base of KIT's recorders
whose clocks held (St. Petersburg above). After the fix, Tallinn sits at +1 s against
this reconstruction in every 6 h window (sub-second peak +0.8 s), as St. Petersburg
does. Before it, a 2019-04 recording matched against the stored IPS/UPS reference could
have come out up to a minute off.

## Coverage

Months fetched while writing the source:

| Month | Share of seconds stored | Stations that contributed |
|---|---|---|
| 2019-03 | 4.2% | EE08, EE04, EE06 |
| 2019-04 | 8.9% | EE08, EE06, EE04 |
| 2019-05 | 10.7% | EE08, EE06, EE04 |
| 2025-01 | 24.5% | PISE, SRGE, TOSE, NOPE |
| 2025-02 (to the 8th) | 8.4% | PISE, SRGE, TOSE |

Spring 2019 is a weak period: `EE01` has no data then. The probes suggest 2018 and
2020–2022 are much better. Each month is about 4 GB of download and about 16 minutes;
the rest is fetched with `GEOLOC_GEOFON_REFRESH=1 geoloc refs fetch seismic_estonia
--grid ips_ups --months 2015-05:2025-02` and pushed to the bucket. The dashboard never
refreshes it.

## Negative results

- The EIDA nodes (GEOFON, ORFEUS, BGR, ETH, INGV, LMU, NIEP, NOA) list nothing at
  ≥ 120 samples/s in Latvia, Lithuania, Belarus, Ukraine, Moldova, Georgia or Armenia
  (Ukraine's `UD` network records at 40 samples/s).
- Elering's public API (`dashboard.elering.ee/api/system`) has a `frequency` field, but
  only every 5 minutes and rounded to 10 mHz: no use for matching.
