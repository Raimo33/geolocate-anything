# ERCOT grid frequency from seismometer hum (`seismic_ercot`)

Code: the tracker and fusion of [`seismic_east`](seismic_east.md)
(`src/refs/seismic.cpp`, `src/refs/seismic_fetch.cpp`); the screened stations are in
`src/refs/seismic_ercot_stations.inc`.

ERCOT keeps its 4 s frequency history but publishes none of it. The only open
measurements are the ETTS exports of 2011–2012 (about 150 days, `ercot_etts`) and KIT's
campaign of 2019-05 (`osf_edr`). `seismic_ercot` reconstructs the frequency from the
60 Hz hum in seismometers in Texas archived at EarthScope: continuously from 2014-02,
and for scattered days of 2009.

## Screening (2026-11-10)

Candidates: the 5,585 vertical channel epochs at ≥ 80 samples/s that EarthScope lists
inside the `ercot` outline (4,322 channels at ≥ 200 samples/s, most of them nodal
arrays deployed for a few weeks).

1. *Direct line.* Every channel at 200–1,000 samples/s was tracked for 20 minutes from
   10:00 UTC on Feb 15 and Aug 15 of every year, 2008–2026 (the 2,700 nodes of the 2014
   arrays were left out). Tracks were smoothed over 12 s and correlated between sites
   more than 2 km apart.
2. *Monthly, 2008–2014.* The channels of the networks that hummed in those years (`X9`,
   `ZW`, `NQ`, `4F`, `US`) were tracked on the 15th of every month.
3. *Folded line.* The 80 and 100 samples/s channels were tracked on Feb 15 of
   1998–2013: EarthScope returned data for at most two of them, without a line.

A channel is listed if it agreed with another site at r ≥ 0.97 at least once and never
sat off the others' mean. 31 channels at 24 sites pass:

| Network | Where | Years | Typical SNR |
|---|---|---|---|
| `ZW` (SMU North Texas) | Irving, Dallas, Azle | 2014-01 → 2024-12 | `ITL1` 51 dB, `IFS3` 36 dB, `AZDA` 35–39 dB, others 14–28 dB |
| `NQ` (NetQuakes) | Azle, Irving | 2013-12 →, 2015–2016 | 17–21 dB |
| `TX` (TexNet) | Permian Basin (`PB46`, `MB23`, `DG01`, `OG01`, `PB02`), Dallas–Fort Worth (`FW09`, `FW16`), Eagle Ford (`EF04`) | 2017 → | 11–28 dB |
| `HZ`, `4O` | Midland Basin | 2026 (stations since 2018) | 15–30 dB |
| `X9` (SMU, DFW airport and Cleburne) | Dallas–Fort Worth | 2009-06 → 2011 | 10–19 dB |

Sites scoring r ≥ 0.9, by year: 4 in 2009, 1 in 2010, none in 2011–2013, 2 in 2014,
5–8 in 2015–2020, 3–4 in 2021–2025, 12 in 2026.

## It is ERCOT's frequency

- *Sites far apart agree at zero lag.* After 12 s smoothing: Pecos (`TX.PB46`) against
  Irving (`ZW.IFS3`), 631 km, r = 0.997 and 1.2 mHz rms (2024-08-15); Azle (`ZW.AZDA`)
  against Irving (`ZW.ITL1`), 62 km, r = 1.000 and 0.16 mHz (2015-02-15); Eagle Ford
  (`TX.EF04`) against the Midland Basin (`HZ.MBNS2`), 538 km, r = 0.973 (2026-08-15).
  The operators differ (SMU, the Bureau of Economic Geology, USGS).
- *It is neither neighbour.* In the same 20 minutes the Texas stations are uncorrelated
  with Eastern stations (`KY.HZKY`, `CO.TEEBA`: r = 0.00 and −0.24) and Western ones
  (`PB.B087`, `NP.2194`: r = −0.43 and +0.28), which agree among themselves at
  r = 0.999–1.000 (2018-02-15 and 2026-02-15).
- *End to end.* 2021-02 was reconstructed with the downloader. A 15-minute synthetic
  clip whose 60 Hz tone follows `TX.FW16` (40 km south of Irving, another network;
  tracked by a separate script) from 2021-02-10 10:00:00 UTC is located at 09:59:59 UTC,
  STRONG. With `FW16` left out of the month (a build that drew only on the Irving
  stations) the score was 53.8 against 12.2 for the best wrong alignment; in the month
  as now built, where `FW16` is a reserve, 53.5 against 11.1.
- *Not yet checked against a recorder.* None of the ETTS days (2011–2012) has a humming
  station. KIT's College Station recordings (2019-05, `osf_edr`) do overlap: comparing
  them with `seismic_ercot` 2019-05 is the test to run once both are in one store.
  `osf_edr` and `ercot_etts` rank above `seismic_ercot` where they overlap.

## Rules that differ from the Eastern Interconnection

- **Two sites must agree from 2015-02.** A strong line is not always the grid's: on
  2025-05-15 `TX.PB46` carried one at 32 dB, with a grid-like spread (15 mHz), that
  correlated at r = 0.4 with the stations that agreed with each other. The Permian
  sites sit in oilfields with their own generation. Before 2015-02 there is often only
  one site (Azle, 30–50 dB), which then stands alone under the Eastern rule (noise
  ≤ 1.5 mHz).
- **Stations with an offset are not listed.** `TX.PB11`, `TX.FW15` and `4O.MBBB6` follow
  the grid's wander (r up to 0.999) but 150–230 mHz below it; `PB11` did so from
  mid-2021 and read true before. Whatever shifts them, they are left out.
- **One site per place.** `ZW.IPD1` and `ZW.IPD1V`, or `TX.FW09` and `ZW.IFBF`, are the
  same building under two codes; stations within 0.5 km of each other count as one
  site, so they cannot confirm each other.

## Coverage and limits

Months reconstructed while writing the source:

| Month | Share of seconds stored | Stations (download) |
|---|---|---|
| 2019-05 | 99.1% | `ZW.IFS3`, `ZW.ITL1`, `ZW.IPD1V`; reserves `TX.PB02`, `TX.FW16` (5.0 GB) |
| 2021-02 | 78.7% | `ZW.IFS3`, `ZW.IPD1V`, `ZW.ITL1`; reserves `TX.FW09`, `TX.FW16` (6.4 GB) |

- 2014-02 onwards; scattered days of 2009-08..12 where two `X9` stations hummed. No
  listed station exists for 2012-01..2013-12, and EarthScope holds nothing usable
  before 2009.
- From 2025 the Dallas network (`ZW`) is closed and the reference rests on the Permian
  and Midland stations, at lower SNR.
- The tracker searches ±250 mHz around 60 Hz. The minutes of 2021-02-15 when ERCOT ran
  below 59.75 Hz are therefore not stored.
- A month is three stations fetched in full and up to two reserves, 5–6 GB (the `ZW`
  channels compress poorly). The rest is fetched offline and pushed to the bucket, never by
  the dashboard: `GEOLOC_US_SEISMIC_REFRESH=1 GEOLOC_EARTHSCOPE_REFRESH_TOKEN=... geoloc
  refs refresh --grid ercot`, then `geoloc refs push`. It shares the refresh lane with
  `seismic_west` (one EarthScope account, 5 connections); don't run it alongside
  `seismic_east` on the same account.

## Other sources looked for (2026-11-10)

ERCOT's public data API lists no frequency product (its only 4 s data is storage
charging power), and its real-time page shows the current value only. The Texas
Synchrophasor Network (Baylor, UT Austin) and ERCOT's own PMU archive (kept 400 days)
share data on request. PingThings' NI4AI open PMU archive has closed. No Zenodo,
figshare, Mendeley or GitHub dataset with measured ERCOT frequency turned up beyond
the two already used. Raspberry Shake's archive (100 samples/s geophones in Texas,
which could show the line folded to 40 Hz) was not reachable from the development
sandbox and remains to be screened.
