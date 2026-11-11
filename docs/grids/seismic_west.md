# US Western grid frequency from seismometer hum (`seismic_west`)

Code: the tracker and fusion of [`seismic_east`](seismic_east.md)
(`src/refs/seismic.cpp`, `src/refs/seismic_fetch.cpp`); the screened stations are in
`src/refs/seismic_west_stations.inc`.

Measured Western Interconnection frequency is open for 2019-01..2025-07 only (Alberta's
`aeso`), plus twelve days of 2015 (`lbnl_upmu`) and KIT's campaigns. `seismic_west`
reconstructs the rest from the 60 Hz hum in seismometers archived at EarthScope:
2001-09..2018-12 and 2025-08 onwards. The months AESO covers are not reconstructed.

## Screening (2026-11-10)

Candidates: the vertical channels of permanent networks that EarthScope lists inside
the `us_western` outline: 4,635 at ≥ 200 samples/s and 7,896 at 80–100 samples/s
(temporary deployments, another 65,000 channel epochs, were left out).

1. *Direct line.* Every ≥ 200 samples/s channel was tracked for 15 minutes from
   10:00 UTC on Feb 15 of 2005–2018 and 2026.
2. *Folded line.* Every 100 samples/s channel (60 Hz folds to 40 Hz) on Feb 15 of
   2000–2012, every second year.
3. *Quarterly probe.* The 139 channels that hummed at ≥ 22 dB or agreed with another
   site were then tracked on the 15th of Feb, May, Aug and Nov of 2001–2018 and
   2025–2026. A station's score for a year is its best r in that year's probes.

Tracks were smoothed over 12 s and correlated between sites more than 2 km apart. A
channel is listed if it agreed with another site at r ≥ 0.97 at least once and never
sat off the others' mean: 112 channels in 14 networks.

| Era | Stations | Line |
|---|---|---|
| 2001–2010 | Short-period `EHZ` at 100 samples/s: `UW.TWW` (47–52 dB every year), `UW.DPW`, `UW.KMO` and others in Washington, `MB.BUT` (Montana), `IE.IRCI` (Idaho), `SC.SBY`, `SC.WTX`, `SC.BAR` (New Mexico), `AR.PGAZ` (Arizona) | folded to 40 Hz |
| 2008–2018 | Borehole strainmeter sites near Anza, California (`PB.B082`–`B093`, 40–64 dB), the Garner Valley array (`SB`), strong-motion sites in Washington, Oregon, Nevada and Utah from 2012 on | direct |
| 2025 → | 60–67 sites a year: `UW`, `UO`, `NN`, `UU`, `NP`, `NV` (Vancouver Island), `PB`, `PY` | direct |

Sites scoring r ≥ 0.9, by year: 2 in 2001, 6–10 in 2002–2007, 11–18 in 2008–2014,
21–32 in 2015–2018, over 60 in 2025–2026.

## It is the Western Interconnection's frequency

- *Sites up to 1,800 km apart agree at zero lag* (12 s smoothing, 15 min):

  | Date | Pair | Distance | r | rms difference |
  |---|---|---|---|---|
  | 2026-02-15 | `NN.VCN` (Nevada) – `PB.B087` (S. California) | 695 km | 0.9999 | 0.18 mHz |
  | 2026-02-15 | `NV.NSMTC` (Vancouver Island) – `PY.PFOIX` (S. California) | 1,746 km | 0.9998 | 0.27 mHz |
  | 2018-02-15 | `UW.MKAH` (Washington) – `PB.B084` (S. California) | 1,744 km | 0.9998 | 0.26 mHz |
  | 2010-02-15 | `IE.IRCI` (Idaho) – `SC.SBY` (New Mexico) | 1,128 km | 0.9990 | 0.62 mHz |
  | 2004-02-15 | `UW.TWW` (Washington) – `MB.BUT` (Montana) | 640 km | 0.9982 | 0.64 mHz |
  | 2002-08-15 | `UW.TWW` – `MB.BUT` | 640 km | 0.9903 | 1.18 mHz |

- *It is neither neighbour.* In the same minutes the Western stations are uncorrelated
  with Eastern and Texas stations ([seismic_ercot.md](seismic_ercot.md)).
- *The old stations keep time.* The 100 samples/s short-period stations are analogue
  sites digitised centrally. Against the GPS-timed boreholes at Anza their best lag is
  within ±0.3 s in almost every probe of 2010–2016 (`UW.TWW`: −0.3 to +0.2 s over 17
  probes; `IE.IRCI`: 0.0 to −0.1 s; `SC.SBY`: mostly within ±0.3 s, once −1.4 s), and
  among themselves in 2002–2008 (`UW.DPW`, `MB.BUT`, `IE.IRCI` against `UW.TWW`: within
  ±0.4 s, one probe at −0.6 s).
- *End to end.* 2006-02 was reconstructed with the downloader from `UW.TWW`, `UW.KMO`
  and `UW.DPW` (92.6% of its seconds). A 12-minute synthetic clip whose 60 Hz tone
  follows `SC.SBY` (New Mexico, 1,800 km away, not used that month; tracked by a
  separate script) from 2006-02-20 18:30:00 UTC is located at 18:29:59 UTC, STRONG:
  score 70.0 against 13.3 for the best wrong alignment.
- *Not yet checked against a recorder.* `lbnl_upmu` (2015-10-01..12) overlaps
  `seismic_west` 2015-10; comparing the two is the test to run once both are in one
  store. Fetching one AESO month (`geoloc refs fetch seismic_west --grid us_western
  --months 2024-04:2024-04`) gives a second one, against AESO and KIT's Colorado
  recorder. `aeso`, `lbnl_upmu` and `osf_edr` rank above `seismic_west` where they
  overlap.

## Rules that differ from the Eastern Interconnection

- **Nobody stands alone.** Among the probes in which a station hummed strongly enough
  for the Eastern lone-station rule (≥ 28.5 dB) while others agreed with each other,
  about one in thirteen did not follow them: `UW.DPW` in five probes, `UW.TWW` and a
  few others once, and `AZ.SCI2` (San Clemente Island, which has its own power plant; not
  listed) in seven. There are always other stations to ask, so a second is stored only where
  two sites agree.
- **One site per place.** The Garner Valley array (`SB.GVA03`, `GVA04`, `GVA06`,
  `GVA08`, `GVDA`) and `PB.B084` with `PY.PFOIX` share coordinates under different
  station codes; stations within 0.5 km of each other count as one site.
- **The AESO months are skipped** (`Network::skip_from`, `skip_to`): 2019-01..2025-07.

## Months reconstructed so far

| Month | Share of seconds stored | Stations (download) |
|---|---|---|
| 2006-02 | 92.6% | `UW.TWW`, `UW.KMO`, `UW.DPW`; reserves `UW.GBL`, `UW.SMW` (2.2 GB) |
| 2015-10 | 100.0% | `PB.B084`, `PB.B087`, `UW.BLN`; reserve `UW.SLA` (1.7 GB) |
| 2025-09 | 100.0% | `NN.VCN`, `NV.NSMTC`, `PB.B082` (3.3 GB) |

The rest, 224 months at about 2 GB each, is fetched offline and pushed to the bucket,
never by the dashboard:

    GEOLOC_US_SEISMIC_REFRESH=1 GEOLOC_EARTHSCOPE_REFRESH_TOKEN=... geoloc refs refresh --grid us_western
    geoloc refs push

## Limits and open questions

- Nothing before 2001-09: EarthScope's continuous archive of the Washington
  short-period network starts then (2000-02-15: 29 channels with data, none agreeing).
- 2001-09..12 rests on two stations (`UW.DPW`, `MB.BUT`).
- The Northern and Southern California archives (NCEDC, SCEDC) hold the `NC`, `BK` and
  `CI` networks, which EarthScope mostly does not; they were not reachable from the
  development sandbox. With over 60 sites today they are not needed for coverage, but
  they could add stations before 2008.
- Other sources looked for (2026-11-10): no open Western frequency archive besides the
  ones already used. BPA, CAISO and the other operators publish none; FNET/GridEye is
  on request; Portland State's open PMU data covers events only; LBNL's later power
  data portal and hobbyist loggers (time-nuts) could not be reached from the sandbox.
