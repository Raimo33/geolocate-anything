# Grid frequency from seismometer hum

No open per-second frequency exists for most of these grids, so they are reconstructed
from the mains hum that seismometers and accelerometers pick up inadvertently.
`seismic_east` came first and holds the method (screening, tracking, fusion);
the other sections describe what differs for each grid.

## Contents

- [US Eastern (`seismic_east`)](#us-eastern-seismic_east)
- [US Western (`seismic_west`)](#us-western-seismic_west)
- [ERCOT (`seismic_ercot`)](#ercot-seismic_ercot)
- [IPS/UPS from Azerbaijan and Central Asia (`seismic_ips`)](#ipsups-from-azerbaijan-and-central-asia-seismic_ips)
- [IPS/UPS from Estonia (`seismic_estonia`)](#ipsups-from-estonia-seismic_estonia)
- [IPS/UPS from Raspberry Shakes (`seismic_ips_shake`)](#ipsups-from-raspberry-shakes-seismic_ips_shake)
- [China (`seismic_cn_*`)](#china-seismic_cn_)
- [Western Australia and Darwin (`seismic_swis`, `seismic_perth_basin`, `seismic_darwin`)](#western-australia-and-darwin-seismic_swis-seismic_perth_basin-seismic_darwin)
- [Brazil (`seismic_brazil`)](#brazil-seismic_brazil)

## US Eastern (`seismic_east`)

Code: `src/refs/seismic.cpp` (tracking and fusion), `src/refs/seismic_fetch.cpp`
(EarthScope downloads), `src/refs/mseed.cpp` (miniSEED reader),
`src/refs/seismic_stations.inc` (screened station list). How it fits the reference store:
[architecture.md](architecture.md#ingestion).

No operator publishes per-second frequency for the US Eastern Interconnection, and the
FNET/GridEye archive is available only on request. `seismic_east` reconstructs it from
the 60 Hz mains hum that seismometers and accelerometers pick up inadvertently.

### Rationale

A sensor or digitiser installed in a building or powered from the mains
couples to the 60 Hz field electrically or mechanically. The digitisers are GPS
disciplined, so every sample is timed to the millisecond, and FDSN data centres
(EarthScope) archive the waveforms continuously and openly. Each such station is
therefore an unintended GPS-timed grid frequency recorder.

### Channel selection and aliasing

Only vertical channels of permanent networks inside
the Eastern Interconnection are considered.
- At ≥ 200 samples/s (most strong-motion `HNZ` and some broadband `HHZ` channels), 60 Hz is
  below Nyquist and observed directly.
- At 100 samples/s the line appears aliased at 40 Hz, with the deviation's sign flipped.
  The tracker folds the nominal frequency into [0, fs/2] and restores the sign, so an
  aliased channel yields the same deviation as a direct one. Rates that fold the line to
  within 2 Hz of 0 Hz or Nyquist (e.g. 120 and 40 samples/s) are unusable.
- Aliasing requires an anti-alias filter that lets 60 Hz through. Modern steep FIR filters
  remove it; the older short-period `EHZ` stations of the Lamont-Doherty network (`LD`)
  do not, with the folded line 26–37 dB above noise. These stations provide the coverage
  before 2014, when few ≥ 200 samples/s channels were archived continuously.

### Screening

All candidate channels (858 at ≥ 200 samples/s, plus 100 samples/s channels
before 2014) were tracked over 15-minute windows sampled each year (quarterly before 2014).
Tracks were smoothed over 12 s and correlated pairwise. A channel is retained if it
reached r ≥ 0.97 with another channel at least once. Agreement is the criterion because
the grid frequency is the only signal all stations share, whereas a motor or power-supply
tone drifts independently. The resulting list holds 67 channels at 55 sites in 13
networks.

### Station choice per month

Each station is scored by its correlation in the nearest
screened year (penalised by 0.9 if more than 3 years away). Up to eight stations with a
score ≥ 0.9 are chosen, at most one channel per site. The best three are fetched for the
whole month; the others are fetched only for 6-hour chunks in which fewer than three
stations supported the fused value.

### Tracking a station

For each contiguous segment:
1. The hum line is mixed to 0 Hz, low-pass filtered (Kaiser FIR, flat to ±1 Hz, −80 dB
   from 3 Hz) and decimated to 10 samples/s in one zero-delay complex FIR.
2. For each UTC second, a 12 s sin²-windowed frame is searched at 5 mHz steps within
   ±0.25 Hz of nominal; the peak is refined by parabolic interpolation. Peaks on the edge
   of the range are rejected.
3. SNR is the peak power against 24 noise probes at 0.6–1.0 Hz on either side.

The 12 s frame matches the frame used on query audio, so reference and query are smoothed
identically.

### Fusing stations

1. A station-second is valid if its SNR is ≥ 8 dB and it lies within ±400 mHz of nominal.
2. Over 10-minute windows (5-minute hop), each station's noise is estimated as
   40 mHz / √SNR, following the 1/√SNR law for a tone's frequency error.
3. Stations whose smoothed tracks correlate at r ≥ 0.9 agree. Stations agreeing with at
   least half as many others as the best-connected station are accepted.
4. If no two stations agree, only the quietest may stand alone, and only if its noise is
   ≤ 1.5 mHz; this guards against equipment tones.
5. Per second, the inverse-variance weighted mean of accepted stations is stored, after
   rejecting outliers farther than max(5 mHz, 4σ) from the weighted median.

### Validation

- *Independent recorder.* Against KIT's recorder in Florida (2023-06/07), single stations
  reach r = 0.997–0.9997 (0.4–1.1 mHz RMS) and the fused series r = 0.996–0.9993
  (0.6–0.9 mHz RMS per day). Four seismic stations in different states and networks agree
  with each other at zero lag, while KIT's lag drifts from −4 s to −13 s; the drifting
  clock is therefore KIT's.
- *Pre-2014 stations.* On 2005-07-15, three `LD` stations at 100 samples/s tracked 95% of
  seconds at median SNRs of 26–38 dB and agreed within about 2 mHz.

### Coverage

Share of seconds with a value, 2026-11-12, after the weak-month backfill (every month below
95%, and all of 2010–2013, fetched again with the rescreened and newly screened channels):

| Years | Coverage | Months below 95% |
|---|---|---|
| 2001 (09–12) | 76% | 2001-09: 41% (only LD.CRNY/MANY/MSNY exist); 2001-11: 93%; 2001-12: 73% |
| 2002–2004 | 92–97% | 2002-01..05: 77–89%; 2002-08: 93%; 2003-09: 85% (was 41%); 2004-03: 92%; 2004-11: 83% |
| 2005–2009 | 97–99% | 2009-03: 92% (2008-05: 59% → 100%) |
| 2010 | 97% | 2010-03: 89% (2010-01..04 were stored as no data, 2010-05 17%) |
| 2011 | 98% | 2011-12: 88% (was 24%) |
| 2012 | 67% (was 1%) | 2012-01..06: 33–75%; 2012-10..12: 53–81% (2012-07..09: 94–98%) |
| 2013 | 91% (was 41%) | 2013-01: 77%; 2013-03: 42%; 2013-05: 93% |
| 2014–2015 | 96–99% | 2015-04: 89%; 2015-05: 80%; 2015-06: 93% |
| 2016–2017 | 96–97% | 2016-02: 73%; 2017-10: 61% (was 35%) |
| 2018–2026 | 97–100% | 2021-10/11: 88% (2021-06: 70% → 100%) |

The per-month figures before and after the backfill are in the
[rescreen section](#rescreen-of-20102013-2026-11-12).

### Rescreen of 2010–2013 (2026-11-12)

The 2010–2013 gaps came from the station choice, not from the archive. EarthScope holds
continuous data for every month of 2010, and stations that hum in 2011 and 2013 hum in 2012
too. One 15-minute probe a quarter had scored them low in 2012 (LD.PAL `HNZ` 0.23, LD.YSLD
0.29, NQ.RSTON 0.43), while channels that had gone quiet kept their 2007/2009 scores
(LD.PAL `EHZ`, MSNY, BVD, NED; their digitisers were replaced in 2009-11 and the folded line
is gone). The picker took the five best-scored channels, mostly dead ones, so nothing passed.

22 candidate channels were tracked twice a month (the 8th at 05:00 and the 22nd at 17:00
UTC, 30 min each) from 2010-01 to 2013-12, plus an all-channel screen (every vertical and
horizontal ≥ 200 samples/s inside the outline) on 2012-04-15 and 2012-06-15. A station's
yearly score is its best r against another site that year. Pairs at one site, and LD.MANY
with LD.TBR (from 2012 they agree only with each other, a shared non-grid tone), do not count.

| Months | Sites that agree (r after 12 s smoothing) |
|---|---|
| 2010-01..12 | MANY, TBR, PAL `HNZ`, BGR: 0.96–1.00 (PAL with MANY 0.99 on 2010-02-15 and 2010-05-15) |
| 2011-09..2012-07 | PAL, RSTON, GS.LWRD (to 2012-03), YC.IP04/IP06 (RAMP Virginia aftershock array): 0.96–0.99 |
| 2012-06..08 | adds CN.MCA3 (McAdam, NB) 0.99–1.00, NQ.ETX02 (East Texas) 0.98 |
| 2012-08..2013-12 | LD.YSLD (Youngstown) with RSTON and PAL: 0.96–0.99; from 2013-02 NE.WES, from 2013-04 NP.7405, from 2013-11 GS.DEC07/09 |

New channels: YC.IP04, YC.IP06, CN.MCA3, NQ.ETX02. Re-scored: LD.PAL `HNZ`, YSLD,
RSTON, LWRD, MANY, TBR, BGR (2010–2013), and LD.PAL `EHZ`, MSNY, BVD, NED set to 0 from
2010. Windows still without two agreeing sites: 2012-03 (PAL alone or quiet in all four
probes), and some evening probes of 2011-12..2012-02 (PAL alone). These months need format 3
(`month_formats` in `src/refs/sources.cpp`), so the next refresh fetches them again, the ones
marked unavailable included.

**Refetched 2026-11-12.** Coverage before → after (%):

| Month | Before | After | Month | Before | After | Month | Before | After |
|---|---|---|---|---|---|---|---|---|
| 2010-01 | 0 | 98.0 | 2011-05 | 96.5 | 97.8 | 2012-09 | 0 | 95.1 |
| 2010-02 | 0 | 97.4 | 2011-06 | 96.5 | 98.6 | 2012-10 | 0 | 80.8 |
| 2010-03 | 0 | 89.3 | 2011-07 | 97.7 | 99.2 | 2012-11 | 0 | 71.7 |
| 2010-04 | 0 | 98.3 | 2011-08 | 97.5 | 99.3 | 2012-12 | 0 | 52.7 |
| 2010-05 | 17.4 | 97.6 | 2011-09 | 97.0 | 99.7 | 2013-01 | 77.4 | 77.4 |
| 2010-06 | 96.5 | 96.0 | 2011-10 | 92.6 | 97.1 | 2013-02 | 99.8 | 99.8 |
| 2010-07 | 99.0 | 99.8 | 2011-11 | 96.7 | 99.8 | 2013-03 | 40.6 | 41.5 |
| 2010-08 | 99.4 | 99.5 | 2011-12 | 23.6 | 88.2 | 2013-04 | 81.2 | 95.4 |
| 2010-09 | 98.7 | 99.3 | 2012-01 | 19.6 | 65.1 | 2013-05 | 30.2 | 93.3 |
| 2010-10 | 99.8 | 98.6 | 2012-02 | 0 | 51.0 | 2013-06 | 14.3 | 97.9 |
| 2010-11 | 92.5 | 98.3 | 2012-03 | 0 | 38.2 | 2013-07 | 14.5 | 98.4 |
| 2010-12 | 96.7 | 95.4 | 2012-04 | 0.2 | 51.1 | 2013-08 | 49.2 | 97.0 |
| 2011-01 | 97.5 | 98.3 | 2012-05 | 0 | 33.4 | 2013-09 | 3.7 | 96.0 |
| 2011-02 | 97.8 | 99.3 | 2012-06 | 0 | 74.9 | 2013-10 | 0.0 | 95.9 |
| 2011-03 | 96.4 | 97.5 | 2012-07 | 0 | 93.9 | 2013-11 | 2.7 | 96.3 |
| 2011-04 | 97.2 | 98.1 | 2012-08 | 0 | 98.5 | 2013-12 | 78.9 | 98.9 |

2012 went from 1% to 67% and 2013 from 41% to 91%. Still under 90%: 2010-03 (MANY, TBR,
PAL `HNZ`), 2011-12 and 2012-01 (PAL `HNZ`, GS.LWRD, YC.IP06), 2012-02..05 (PAL `HNZ` and
YC.IP06 only, about 1–1.3 M seconds each: two sites, so every gap in either is a gap in the
store), 2012-06 (CN.MCA3, PAL, RSTON, IP06, ETX02), 2012-10..12 and 2013-01 (YSLD, RSTON,
PAL; NE.WES joins in 2013-01), and 2013-03 (YSLD, RSTON, PAL, NP.7405, YC.LA10). NQ.RSTON
contributed nothing from 2012-01 to 2012-05, and LD.BGR nothing from 2012 on.

The weak months outside 2010–2013 were fetched again the same day, with the channels from
the vertical-channel screen:

| Month | Before | After | Month | Before | After | Month | Before | After |
|---|---|---|---|---|---|---|---|---|
| 2001-09 | 41.0 | 41.0 | 2004-02 | 90.9 | 95.5 | 2014-07 | 90.6 | 97.5 |
| 2001-11 | 92.9 | 92.9 | 2004-03 | 86.6 | 91.9 | 2014-08 | 71.6 | 99.8 |
| 2001-12 | 73.2 | 73.2 | 2004-11 | 82.7 | 82.7 | 2015-04 | 88.6 | 89.0 |
| 2002-01 | 86.5 | 87.6 | 2007-06 | 94.8 | 94.8 | 2015-05 | 76.7 | 80.2 |
| 2002-02 | 78.5 | 83.6 | 2008-01 | 94.3 | 98.8 | 2015-06 | 72.4 | 93.3 |
| 2002-03 | 70.9 | 77.2 | 2008-02 | 91.1 | 99.9 | 2015-07 | 60.5 | 98.5 |
| 2002-04 | 82.2 | 83.4 | 2008-03 | 94.3 | 99.7 | 2015-08 | 78.3 | 99.8 |
| 2002-05 | 87.5 | 89.2 | 2008-04 | 91.4 | 99.5 | 2015-09 | 87.3 | 98.9 |
| 2002-08 | 91.8 | 92.5 | 2008-05 | 59.4 | 99.8 | 2016-02 | 62.4 | 72.8 |
| 2003-08 | 92.8 | 97.3 | 2008-08 | 93.6 | 96.1 | 2016-07 | 91.0 | 100.0 |
| 2003-09 | 41.0 | 85.4 | 2009-03 | 92.1 | 92.3 | 2016-08 | 68.4 | 100.0 |
| | | | 2009-09 | 95.0 | 97.9 | 2016-09 | 68.8 | 100.0 |
| | | | 2009-10 | 87.3 | 95.7 | 2017-04 | 92.2 | 98.0 |
| | | | 2009-12 | 94.6 | 96.8 | 2017-10 | 35.1 | 60.9 |
| | | | | | | 2021-06 | 70.2 | 100.0 |
| | | | | | | 2021-10 | 77.6 | 87.9 |
| | | | | | | 2021-11 | 83.5 | 88.2 |
| | | | | | | 2022-08 | 93.6 | 99.6 |

Still under 90% there: 2001-09 and 2001-12 (only LD.CRNY, MANY, MSNY; MSNY gave nothing in
2001-12), 2002-01..05 (CRNY, MANY, GPD, PAL `EHZ`; MSNY mostly silent, ARNY and TBR gave
nothing), 2003-09 and 2004-11 (the same LD set), 2015-04/05 (NP.7405, YC.LA10, ZA.WK05/06;
KY.HZKY, BHKY and GS.KAN16 gave nothing), 2016-02 (YC.LA10/LA17, ZA.WK05), 2017-10
(KY.BHKY, ZA.WK05, N4.T45B, GS.KAN16; KY.HZKY gave nothing), and 2021-10/11 (KY.HZKY,
YU.S13A, WU.TOBO, plus GS.DEC01 in 2021-11).

The same weakness shows elsewhere: the all-channel screen of 2015-06-15 (a 60–78% month)
found 30-odd Eastern channels humming, many unlisted (GS.KAN05/06/08/13/17 in Kansas,
YC.LA10 in Louisiana, NQ.NLKCP, horizontals of KY.HEKY), and 2017-10-15 (35%) over a dozen
listed sites at r ≥ 0.98 (GS.DEC08/09/17, NP.7405, N4.T45B, KY.HZKY, PE.ALL05, NQ.RSTON).
A vertical-channel screen of every weak month (the 8th at 05:00 and the 22nd at 17:00 UTC,
20 min, 2026-11-12) added the channels that agree with listed stations at r ≥ 0.97 in two
windows or more, one or two per cluster: CO.CSB (Charleston), GS.DEC15 (Decatur),
KY.SOKY, WU.PKRO (Ontario), Y7.PW01 (Pawnee OK), YC.LA10/LA17 (Napoleonville LA),
YU.S13A (Ithaca NY), Z7.LA13 (NW Louisiana), ZA.WK05/WK06 (Wellington KS), with their
yearly r (0.97–1.00). The weak months before 2010 (2001-09, 2002-01..05, 2003-09, 2008-05,
2009-10) showed no unlisted channel: only listed LD stations hum, and in some windows none.

### Negative results

- *Harmonics.* The 120 and 180 Hz harmonics are absent (0–5 dB, against 29–40 dB at 60 Hz),
  including on a 500 samples/s channel, so they cannot fill gaps. Remaining gaps come from
  the lone-station rule.
- *Other regions.* The same screening found no usable network in China, Brazil, Western
  Australia, Québec or Taiwan, nor among Raspberry Shakes or modern 100 samples/s networks
  (including Japan's Hi-net and K-NET). The method needs continuously archived channels
  that see the line at sites far enough apart to confirm each other. Besides the eastern
  US, IPS/UPS has them since 2022: [#ipsups-from-azerbaijan-and-central-asia-seismic_ips](#ipsups-from-azerbaijan-and-central-asia-seismic_ips). In China the
  Raspberry Shakes do carry the 50 Hz line, on the Nyquist frequency of their
  100 samples/s, where this tracker cannot read it; a separate decoder does:
  [#china-seismic_cn_](#china-seismic_cn_).
- *Québec and Newfoundland, from NRCan's own archive* (screened 2026-11-09). EarthScope
  holds few Canadian channels, so the 837 vertical channels inside the `quebec` and
  `newfoundland` outlines were also requested from Earthquakes Canada's FDSN service
  (`earthquakescanada.nrcan.gc.ca/fdsnws`; networks CN, QW, QX, PO, C8). It archives
  only 100 samples/s data continuously: QW's 250 samples/s `CNZ` channels and the older
  200 samples/s CN `HNZ` channels return nothing. On 2026-02-15 (196 channels with
  data, 1 h) four QW sites hundreds of kilometres apart (QCL05, QCL06, QCS14, QCM23)
  share a ~10 mHz wander uncorrelated with the Eastern stations (r ≈ 0), probably
  Québec's frequency. But they agree at a median r of only 0.4–0.7 per 10 min (Eastern
  stations: 0.997). The pairs that agree closely are co-located and do not follow the
  grid: CN.OTT with QW.ONN41 (Ottawa, r = 0.998, but r ≈ 0.1–0.2 against the Eastern
  stations) and CN.MNTQ with QW.QCM01 (same Montréal coordinates). On 2015-02-15
  (73 channels) no pair exceeded r = 0.14. Newfoundland's three CN stations show no
  line. Too weak for a reference.

### Open questions

How the hum couples into each instrument; why a station's agreement
varies from year to year; accuracy against a PMU archive (FNET/GridEye) over a longer
period; whether older networks extend coverage before 2001 (1975-01..1984-07 were fetched and nothing passed; 1984-08..2000 was not tried, since no listed station existed then); whether monthly screening lifts the remaining weak months (2015, 2016, 2017-10, 2021).

## US Western (`seismic_west`)

Code: the tracker and fusion of [`seismic_east`](#us-eastern-seismic_east)
(`src/refs/seismic.cpp`, `src/refs/seismic_fetch.cpp`); the screened stations are in
`src/refs/seismic_west_stations.inc`.

Measured Western Interconnection frequency is open for 2019-01..2025-07 only (Alberta's
`aeso`), plus twelve days of 2015 (`lbnl_upmu`) and KIT's campaigns. `seismic_west`
reconstructs the rest from the 60 Hz hum in seismometers archived at EarthScope:
2001-09..2018-12 and 2025-08 onwards. The months AESO covers are not reconstructed.

### Screening (2026-11-10)

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

### It is the Western Interconnection's frequency

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
  with Eastern and Texas stations ([#ercot-seismic_ercot](#ercot-seismic_ercot)).
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

### Rules that differ from the Eastern Interconnection

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

### Months reconstructed so far

| Month | Share of seconds stored | Stations (download) |
|---|---|---|
| 2006-02 | 92.6% | `UW.TWW`, `UW.KMO`, `UW.DPW`; reserves `UW.GBL`, `UW.SMW` (2.2 GB) |
| 2015-10 | 100.0% | `PB.B084`, `PB.B087`, `UW.BLN`; reserve `UW.SLA` (1.7 GB) |
| 2025-09 | 100.0% | `NN.VCN`, `NV.NSMTC`, `PB.B082` (3.3 GB) |

The rest, 224 months at about 2 GB each, is fetched offline and pushed to the bucket,
never by the dashboard:

    GEOLOC_US_SEISMIC_REFRESH=1 GEOLOC_EARTHSCOPE_REFRESH_TOKEN=... geoloc refs refresh --grid us_western
    geoloc refs push

### Limits and open questions

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

## ERCOT (`seismic_ercot`)

Code: the tracker and fusion of [`seismic_east`](#us-eastern-seismic_east)
(`src/refs/seismic.cpp`, `src/refs/seismic_fetch.cpp`); the screened stations are in
`src/refs/seismic_ercot_stations.inc`.

ERCOT keeps its 4 s frequency history but publishes none of it. The only open
measurements are the ETTS exports of 2011–2012 (about 150 days, `ercot_etts`) and KIT's
campaign of 2019-05 (`osf_edr`). `seismic_ercot` reconstructs the frequency from the
60 Hz hum in seismometers in Texas archived at EarthScope: continuously from 2014-02,
and for scattered days of 2009.

### Screening (2026-11-10)

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

### It is ERCOT's frequency

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

### Rules that differ from the Eastern Interconnection

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

### Coverage and limits

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

### Other sources looked for (2026-11-10)

ERCOT's public data API lists no frequency product (its only 4 s data is storage
charging power), and its real-time page shows the current value only. The Texas
Synchrophasor Network (Baylor, UT Austin) and ERCOT's own PMU archive (kept 400 days)
share data on request. PingThings' NI4AI open PMU archive has closed. No Zenodo,
figshare, Mendeley or GitHub dataset with measured ERCOT frequency turned up beyond
the two already used. Raspberry Shake's archive (100 samples/s geophones in Texas,
which could show the line folded to 40 Hz) was not reachable from the development
sandbox and remains to be screened.

## IPS/UPS from Azerbaijan and Central Asia (`seismic_ips`)

Code: the same as [`seismic_east`](#us-eastern-seismic_east) (`src/refs/seismic.cpp`,
`src/refs/seismic_fetch.cpp`); the screened stations are in
`src/refs/seismic_ips_stations.inc`.

No operator of the IPS/UPS synchronous area (Russia, Belarus, Kazakhstan, Central Asia,
the South Caucasus; Ukraine and Moldova until 2022-03-16, the Baltic states until
2025-02-08) publishes per-second frequency. The only open measurements are KIT's
campaigns: St. Petersburg and Tallinn in 2019-03..05 (`osf_edr`) and Kaunas and Tallinn
in 2025-01-23..02-08 (`osf_baltic`). `seismic_ips` reconstructs the frequency from
2022-10 on from the 50 Hz hum in strong-motion recordings archived at EarthScope, the
way `seismic_east` does for the US Eastern Interconnection.

### What differs from the Eastern Interconnection

- **100 samples/s channels are useless.** At 50 Hz mains the line sits exactly on the
  Nyquist frequency of a 100 samples/s channel, which is what almost every broadband
  station records. Only ≥ 120 samples/s channels see the line directly; 40 and 80
  samples/s channels could show it folded to 10 and 30 Hz.
- **Few stations, and they hum only part of the time.** A station's hum comes and goes
  from month to month. More stations are therefore fetched per month (4 for the whole
  month, up to 6 with the reserves; Eastern: 3 and 5).
- **No station is believed alone.** `AB.SUMQ` at times runs on something that is not the
  grid (2024-10-15: 19 dB SNR, mean −127 mHz, standard deviation 111 mHz; 2026-06-15: 20
  minutes off, then back), presumably a generator. A second site must agree (r ≥ 0.9
  in the 10-minute window) for a second to be stored.

### Screening (2026-11-09)

Candidates: the 1,346 vertical channels at ≥ 40 samples/s that EarthScope lists inside
the `ips_ups` outline (672 stations; networks AB, GO, II, IU, IM, KN, KR, KZ, QZ, RU,
TF, TJ and temporary deployments).

1. *Direct line and folded line.* Every channel whose rate shows 50 Hz away from 0 Hz
   and Nyquist (627 channels: 200 samples/s `HNZ`, 40 and 80 samples/s `BHZ`/`SHZ`/`EHZ`)
   was tracked for 20 minutes from 04:55 UTC on Feb 15 and Aug 15 of every year,
   1994–2026. Pairs of different sites were compared after 60 s smoothing.
2. *Monthly probe.* The channels that agreed with another site at least once were then
   tracked for 30 minutes from 04:45 UTC on the 15th of every month, 2018-10..2026-10.
   A station's score for a year is the share of that year's probes in which it agreed
   with another site at r ≥ 0.9 after 60 s smoothing.

Result: nine 200 samples/s strong-motion channels in three networks, all installed
since 2021:

| Station | Place | From | Typical SNR when humming |
|---|---|---|---|
| `AB.SUMQ.20.HNZ` | Sumqayıt, Azerbaijan | 2022-11 | 22–35 dB |
| `QZ.BRZS.20.HNZ` | Karaganda region, Kazakhstan | 2022-08 | 15–34 dB |
| `KR.CHVK.20.HNZ` | southern Kyrgyzstan | 2024-06 | 13–35 dB |
| `KR.TOKL.20.HNZ` | Toktogul, Kyrgyzstan | 2021-10 | 10–33 dB |
| `KR.ARK.20.HNZ` | Jalal-Abad region, Kyrgyzstan | 2022-09 | 9–14 dB |
| `KR.BTK.20.HNZ` | Batken, Kyrgyzstan | 2021-10 | 9–23 dB |
| `KR.OHH.20.HNZ`, `KR.SFK.20.HNZ` | Kyrgyzstan | 2021-10, 2022-04 | 10–15 dB, one probe each |
| `QZ.USHN.20.HNZ` | East Kazakhstan | 2023-06 | 10 dB, one probe |

Sumqayıt is 2,000 km from Karaganda and from the Kyrgyz stations; the three networks
have different operators. When their hum is present it is the same curve: on
2026-02-15 03:00–05:00 UTC, SUMQ, BRZS and CHVK had the same mean (+5.1, +5.1, +5.2 mHz)
and standard deviation (15–18 mHz), r = 0.71–0.88 second by second and r = 1.00 with a
1 mHz difference after 60 s smoothing. This also confirms that Azerbaijan, Kazakhstan
and Kyrgyzstan ran synchronously on every date where two of them hummed.

### Coverage

Months of the probe in which two sites agreed: 2022-11..2023-02, 2023-07, 2024-01,
2024-11, 2025-01..08, 2025-11..2026-04, 2026-06..10. In the others (2023-03..06,
2023-08..12, 2024-02..10, 2024-12, 2025-09/10, 2026-05) at most one station hummed in
the probe's half hour; such months may still hold days where two do.

Months reconstructed in full while writing the source (anonymous EarthScope quota,
about 7 minutes each), chosen to span the good and the bad:

| Month | Share of seconds stored | Stations that contributed (share of the month) |
|---|---|---|
| 2022-11 | 35.9% | SUMQ 35%, BRZS 27%, ARK 7%, TOKL 5% |
| 2023-01 | 74.3% | SUMQ 73%, BRZS 52%, ARK 26%, OHH 21%, SFK 4%, TOKL 1% |
| 2023-05 | 5.8% | SUMQ 6%, TOKL 3%, ARK 1%, OHH 1% |
| 2024-01 | 14.8% | BRZS 11%, SUMQ 10%, ARK 5%, TOKL 3% |
| 2025-04 | 98.1% | SUMQ 94%, BRZS 84%, CHVK 83%, TOKL 60%, BTK 39% |

So the reference is continuous from late 2024, substantial in the winter of 2022/23 and
thin in between, as the probe suggested.

The rest is fetched by `geoloc refs refresh --grid ips_ups` with
`GEOLOC_EARTHSCOPE_REFRESH_TOKEN` set (about 3–4 GB per month).

### Validation

- *Between sites*: as above; the fusion stores only seconds on which two sites agree.
- *End to end*: a 12-minute synthetic clip whose 50 Hz tone follows BRZS alone on
  2025-04-15 from 04:40:00 UTC (tracked by a separate script, not the code that built
  the reference) is located in the stored 2025-04 at 04:39:59 UTC, STRONG, r = 0.963;
  the best wrong alignment scores 11.7 against 87.6.
- *Not yet checked against a recorder.* KIT's Kaunas and Tallinn recordings
  (2025-01-23..02-08, `osf_baltic`) overlap months in which BRZS hums at 26–27 dB. That
  comparison (same grid, 3,500 km apart) is the test to run once both are in one store;
  `osf_baltic` ranks above `seismic_ips` where they overlap.
- *Timing between sites.* The best lag between two stations is not always zero: within
  ±0.7 s on 2025-04-15, but SUMQ led BRZS and CHVK by 1.9 s on 2026-06-15 (r = 0.987)
  while BRZS and CHVK agreed within 0.2 s. Whether that is a station clock or the
  grid's own dynamics between the Caucasus and Central Asia is open; until it is
  settled the reference is good to about ±2 s.

### Negative results

- *Before 2022-10: nothing at EarthScope.* No channel of any year from 1994 to 2021
  agreed with another site. The 40 samples/s channels of the Kazakh arrays (Karatau,
  Kurchatov, Borovoye, Akbulak, Makanchi) and of the GSN stations in Russia show at most
  a faint folded line (≤ 10–14 dB), and it does not follow the grid: stacking the
  spectra of all elements of an array (10 at Karatau, 21 at Kurchatov, 2015-02-15)
  leaves the arrays uncorrelated with each other (r ≈ 0 up to 120 s smoothing).
  Co-located elements do agree (`KZ.KK02`–`KZ.KKAR`, r = 0.80–0.97 in 2014–2021), as in
  Québec, which says nothing about the grid.
- *Kurchatov 1999–2000.* Three elements of the Kurchatov array (40.0625 samples/s) share
  a line with a grid-like wander (r = 0.87–0.98 with each other; mean −107 mHz in
  1999-02, +10 mHz in 2000-02, standard deviation 19–30 mHz). One site, no second one to
  confirm it, and northern Kazakhstan ran apart from Russia for part of those years.
- *Sub-harmonics.* Synchronous machines turn at the mains frequency divided by their
  pole pairs, so a 100 samples/s channel next to one could carry the grid at 25, 16.7 or
  12.5 Hz. Lines at 50/2 … 50/8 Hz were tracked on all 1,346 channels on the same dates.
  The strongest, 12.5 Hz at `II.LVZ` (Lovozero, Russia; 20–32 dB in most years since
  2007), wanders by 15–45 mHz when scaled to 50 Hz but is uncorrelated with the grid
  (r = −0.07 against SUMQ, BRZS and CHVK on 2025-05-15, which agree with each other at
  0.98–0.99): an asynchronous machine. None of the others agreed with another site.
- *Fixed tones.* Exactly 10.000 Hz on every Kyrgyz `KN` station in 1995–1997 (50–65 dB,
  1 mHz wide): the digitiser, not the mains.
- *`AB.ZRNV`* shows a steady 13–15 dB line with a grid-like spread (11–21 mHz) in
  2024–2025 that is uncorrelated with every other station: not used.

### Ukraine

Ukraine and Moldova have been part of Continental Europe since 2022-03-16, so a
recording from government-held Ukraine after that date is matched against the
Continental European references, which are continuous. What stays uncovered:

- Ukraine before 2022-02-24 (IPS/UPS: from 2015-05 by `seismic_estonia` where Estonian sites hum, before that nothing), and the island
  operation of 2022-02-24..03-16.
- Crimea since 2015 and other areas reconnected to the Russian system: IPS/UPS, covered
  by `seismic_ips` from 2022-10 where two stations agree.
- EarthScope holds two stations in Ukraine at a usable rate (`IU.KIEV`, 40 and 80
  samples/s until 2017): no line.

### Open questions

- *Answered 2026-11-12* ([#ipsups-from-estonia-seismic_estonia](#ipsups-from-estonia-seismic_estonia)): of the European data
  centres, only GEOFON's Estonian network (EE, 250 samples/s since 2015-05) carries the
  line; it is `seismic_estonia`, IPS/UPS until 2025-02-08. No EIDA node lists anything at
  ≥ 120 samples/s in Latvia, Lithuania, Belarus, Ukraine, Moldova, Georgia or Armenia. Elering's public API
  has a frequency field, but at 5-minute steps rounded to 10 mHz.
- Whether the months with a single humming station can be used with a plausibility test
  on that station (spread and mean of the track) instead of a second site. Partly
  answered by [`seismic_ips_shake`](#ipsups-from-raspberry-shakes-seismic_ips_shake): a
  Shake is now that second site.

## IPS/UPS from Estonia (`seismic_estonia`)

Code: the same as [`seismic_ips`](#ipsups-from-azerbaijan-and-central-asia-seismic_ips) (`src/refs/seismic.cpp`,
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

### Screening (2026-11-12)

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

### Validation

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

### KIT's 2019 Tallinn recorder

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

### Coverage

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

### Negative results

- The EIDA nodes (GEOFON, ORFEUS, BGR, ETH, INGV, LMU, NIEP, NOA) list nothing at
  ≥ 120 samples/s in Latvia, Lithuania, Belarus, Ukraine, Moldova, Georgia or Armenia
  (Ukraine's `UD` network records at 40 samples/s).
- Elering's public API (`dashboard.elering.ee/api/system`) has a `frequency` field, but
  only every 5 minutes and rounded to 10 mHz: no use for matching.

## IPS/UPS from Raspberry Shakes (`seismic_ips_shake`)

Code: `src/refs/seismic.cpp` (`track_nyquist`, `fuse` with `FuseOptions::nyquist`),
`src/refs/seismic_fetch.cpp` (network `ips_shake()`, `Network::mixed`); the screened Shakes
are in `src/refs/seismic_ips_shake_stations.inc`, followed by `seismic_ips`'s nine stations.

`seismic_ips` and `seismic_estonia` hum only part of the time, and a second is stored only
where two of their sites agree: on the live dashboard (2026-11-13) IPS/UPS held 52% of
the seconds from 2015-06 on, with 2023-09..2024-10 at 2–49% a month. Raspberry Shakes are
far more numerous in the area (245 registered, most in Almaty, Bishkek, Tashkent, Astana,
Tbilisi, Belarus, Estonia), and their 100 samples/s geophones carry the line on Nyquist,
which `track_nyquist` decodes as for China. `seismic_ips_shake` fuses the Shakes with
`seismic_ips`'s strong-motion stations in one network, so that a Shake and a direct
station confirm each other.

### Fusion of the two kinds

- **Direct stations decide.** Their line has no sign ambiguity. In a 10-minute window where
  a direct station agrees with anyone, only the direct stations in agreement and the
  Shakes that agree with one of them (r ≥ 0.7) are used.
- **Mirrored Shakes.** The Nyquist decoder now and then gets a whole stretch's sign
  wrong, the same way at every Shake. A Shake that anticorrelates with a direct station
  (r ≤ −0.7) confirms that station and is itself left out.
- **Elsewhere** two Shakes more than a kilometre apart that agree are believed, and a lone
  Shake when its line is 13 dB or more (9 mHz by the fusion's noise law) and its track
  wanders by at least 2 mHz (standard deviation in the window; an inverter's fixed
  50 Hz does not). A direct station never stands alone (`AB.SUMQ`'s generator).
- **Requests**: Shakes by the day from the Raspberry Shake archive (one day probed
  first; a silent Shake is not asked for the rest), direct stations by 6 h from
  EarthScope, at most four at once. Every listed station online in the month is used.

### Screening (2026-11-13)

All 245 Raspberry Shakes whose position lies inside IPS/UPS (Ukraine and Moldova until
2022-02-24, the Baltic states until 2025-02-08), with `seismic_ips`'s nine stations, were
decoded for 2 h from 02:00 UTC on the 15th of every month, 2018-01..2026-10 (106 windows).
A Shake is listed if, in at least one window, it agreed (r ≥ 0.7 after 12 s smoothing)
with a direct station or with another Shake more than a kilometre away: 20 do. Shakes that
never agreed are left out even when their line was strong (`AM.RFCF5` hummed alone through
2018-10..2019-01; nothing could confirm it).

Online Shakes are few before 2024: at most four with data in any window of 2018–2023
(Tashkent's `R39E4`, `RC55B`, `R86F9` from 2021; Estonia's `S7560`, `R7B04`, `S5D35`), 5–10
from 2024-09 (Almaty, Bishkek, Astana, Tbilisi, Belarus).

### Validation

- *Against the direct stations*, in every window where two direct stations agreed: 144
  station-windows of the 16 listed Shakes that overlapped them, median r = 0.945 (86% at
  r ≥ 0.9), median RMS 4.8 mHz, sign wrong in a median 3% of the seconds beyond ±10 mHz.
  On 2026-02-15 03:00–05:00 UTC seven Shakes (Almaty, Astana, Belarus, Tbilisi) reached
  r = 0.95–0.98, 3.2–4.5 mHz RMS, against the median of SUMQ, BRZS and CHVK.
- *Failures seen*: `R6995` mirrored for 20 minutes on 2024-10-15 (+18 against −19 mHz),
  while `AB.SUMQ` ran on its generator at −220 mHz; BRZS and `R1B7B` agreed, so the fusion
  kept them and dropped both. Estonian Shakes after 2025-02-08 are uncorrelated with
  IPS/UPS (r 0.2–0.3), as they must be: their epochs end there.

### Coverage

Fusing the probe windows the way each source would (the project's `fuse`, listed
stations only), the share of seconds stored:

| Year | `seismic_ips` alone | `seismic_ips_shake` |
|---|---|---|
| 2018–2020 | 0% | 0–1% |
| 2021 | 0% | 10% |
| 2022 | 14% | 77% |
| 2023 | 24% | 98% |
| 2024 | 23% | 96% |
| 2025 | 89% | 100% |
| 2026 | 96% | 100% |

The windows are at night (02:00 UTC), when hum is strongest; whole months come out
lower. `seismic_ips_shake` ranks below `seismic_ips` and `seismic_estonia` (direct lines,
GPS time) and fills only what they leave.

### Producing the data

Like the other Shake sources: never on the dashboard (`GEOLOC_SHAKE_REFRESH`), backfilled
offline and pushed. The decoding costs ~25 CPU-minutes per humming Shake and month;
`GEOLOC_EARTHSCOPE_REFRESH_TOKEN` gives the direct stations the account's quota.

    GEOLOC_SHAKE_REFRESH=1 geoloc refs refresh --grid ips_ups   # every month not stored yet
    geoloc refs push

### Limits

- *Accuracy*: a Shake is good to about 5 mHz against ~1–2 mHz for a direct line.
- *Mirrored stretches* where only Shakes hum are not caught (as in China).
- *Before 2021 nothing is added*: the Shakes then online never agreed with anyone.
- *Lone Shakes* are believed on their own line; no second site checks them.

## China (`seismic_cn_*`)

Code: `src/refs/seismic.cpp` (`track_nyquist`, and the fusion shared with
[`seismic_east`](#us-eastern-seismic_east)), `src/refs/seismic_fetch.cpp` (downloads), station
lists in `src/refs/seismic_cn_*_stations.inc`.

No Chinese operator publishes per-second frequency, and no open measurement campaign
has recorded in China. Mainland China is not one grid but seven synchronous areas tied
to each other by HVDC only, so each needs its own reference. Four of them are
reconstructed here from the 50 Hz hum in citizen-run Raspberry Shake seismographs.

### The grids

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

### Why the earlier screening found nothing

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

### A line on Nyquist

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

### Validation

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

### Coverage

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

### Producing the data

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
      geoloc refs push                                                   # with the bucket's GEOLOC_GRID_* / GEOLOC_S3_* set

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

### Limits

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
  Australia against a direct line, [#western-australia-and-darwin-seismic_swis-seismic_perth_basin-seismic_darwin](#western-australia-and-darwin-seismic_swis-seismic_perth_basin-seismic_darwin)); now
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

### Other routes examined

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

### Beyond China

Nothing here is specific to China. Any 50 Hz grid with humming Raspberry Shakes can be
reconstructed the same way (Western Australia and Darwin are:
[#western-australia-and-darwin-seismic_swis-seismic_perth_basin-seismic_darwin](#western-australia-and-darwin-seismic_swis-seismic_perth_basin-seismic_darwin); Japan East, IPS/UPS before 2022, India and
South America's 50 Hz grids are candidates); 60 Hz grids fold the line to 40 Hz, which `track_hum`
already reads.

## Western Australia and Darwin (`seismic_swis`, `seismic_perth_basin`, `seismic_darwin`)

Code: the same as [`seismic_cn_*`](#china-seismic_cn_) for the Raspberry Shakes
(`track_nyquist` in `src/refs/seismic.cpp`) and as [`seismic_east`](#us-eastern-seismic_east) for
the 2017 array (`track_hum`); downloads in `src/refs/seismic_fetch.cpp`; station lists in
`src/refs/seismic_swis_stations.inc`, `seismic_perth_basin_stations.inc` and
`seismic_darwin_stations.inc`.

AEMO's frequency data (`aemo_fpp`, `aemo_causer_pays`, `aemo_annual`) covers the NEM:
Queensland to South Australia, and Tasmania. The western half of the continent runs on
grids of its own, none tied to the NEM or to each other, and none of their operators
publishes frequency:

| Grid id | Area | Reference |
|---|---|---|
| `australia_swis` | South West Interconnected System: Kalbarri to Albany, east to Kalgoorlie (Perth) | `seismic_perth_basin`, 2017-01-25..2017-06 (a direct line); `seismic_swis`, 2022-02 on, one station in 2020 |
| `darwin_katherine` | Darwin–Katherine Interconnected System | `seismic_darwin`, one station: spells of 2018–2022, 2024-08 on |
| `pilbara` | North West Interconnected System | none |
| `mount_isa` | North-West Queensland | none |
| `alice_springs` | Alice Springs | none |

### What was searched (2026-11-10)

- **Measured frequency: nothing.** AEMO's WEM data portal holds dispatch and
  half-hourly data, no frequency; AEMO's yearly frequency workbooks are mainland NEM and
  Tasmania only. Western Power, Horizon Power and Power and Water (NT) publish none.
  Zenodo, OSF, figshare: no recording from Perth or Darwin (the "Australia" of the KIT
  and Jülich releases is not in the files). `data.wa.aemo.com.au` itself could not be
  reached from the development sandbox; the above rests on its index as search engines
  show it.
- **Geoscience Australia's stations (network `AU`, at EarthScope): no line.** The
  200 samples/s channels that EarthScope holds from 2021/22 (Gingin, Kellerberrin,
  Mundaring, Rocky Gully, Coolgardie, Kambalda, three sites around Kalgoorlie) show 4–6 dB
  at 50 Hz on eighteen dates in 2021–2026: noise. The 40 samples/s
  channels (50 Hz folded to 10 Hz; Perth's `MTKN` and `CARL`, Mundaring, Ballidu, Morawa,
  Narrogin, and Darwin's `DPH`, `DRS` and Manton Dam) show at most 4 dB (one two-hour
  window, 2026-06-15).
  Two stations do carry a line, neither the grid's: `AU.MENZ` (Menzies, a town with its
  own power station outside the SWIS; 8–21 dB, 9–45 mHz of spread, r = −0.04 against
  Perth) and `AU.KLGHS` (near Kalgoorlie; a steady line 178 mHz below 50 Hz, a machine).
- **Seismometers in schools (network `S1`, 100 samples/s): the line is filtered out.**
  At Busselton, Kalgoorlie, Karratha and Katherine the spectrum at Nyquist lies 12–14 dB
  *below* its neighbourhood, and the decoder returns nothing at any of fifteen schools.
  That rules out the only stations in the Pilbara (Karratha), Katherine and Alice Springs.
- **Raspberry Shakes: yes.** 22 units with a 100 samples/s geophone are registered in
  the South West and three in Darwin (one of them stood in both). Their line sits on Nyquist, as in China, and the
  same decoder reads it.

- **AusPass (the ANU's archive of temporary deployments): one array, in 2017.** It holds
  two dense 250 samples/s arrays inside the SWIS, where the line is seen directly.
  The *Perth Basin Array* (network `5C`, 2017) carries the grid: `seismic_perth_basin`,
  below. The *South West Australia Network* (`2P`, 35 sites, 2020-08..2023-09), probed
  on the 15th of every month, does not: its humming stations sit at exactly 50 Hz
  (`SWN26`: 22–40 dB, 1–5 mHz of spread; `SWN13` and `SWN21` flat to 3 mHz of each other
  once smoothed over a minute, uncorrelated), the inverters of their own solar supplies,
  or wander on a generator (`SWN16` at +109 to +171 mHz). One of them, `SWN22`, was on
  the grid on two of the dates, which is what the Shake decoder is checked against below.
  AusPass has nothing at a usable rate in Darwin, the Pilbara, Mount Isa or Alice Springs.

Not checked: Geoscience Australia's own archive. Its station metadata lists
200 samples/s strong-motion channels in Perth (`AU.CARL`, `AU.MTKN`, `AU.MUN`) and Darwin
(`AU.DPH`, `AU.DRS`) since 2009 that neither EarthScope nor AusPass serves. If they can
be had and a building hums, they would give a direct line for the years in between.

### The Perth Basin Array, 2017 (`seismic_perth_basin`)

Thirty broadband sites across the Perth Basin (Murdie, Yuan & Dentith 2017,
[doi:10.7914/nx28-a367](https://doi.org/10.7914/nx28-a367)), 250 samples/s, GPS-timed,
recorded 2017-01-23..11-29 in two deployments. Screened for 30 minutes from 04:45 and
16:45 UTC on the 5th, 15th and 25th of every month (62 windows): a station's score is
the share of its windows in which it agreed with another site at r ≥ 0.9 after 60 s
smoothing.

| Station | Place | Line when humming | Windows humming / with data | Agreed with another site |
|---|---|---|---|---|
| `5C.PBS15` | Regans Ford (31.00° S) | 7–35 dB | 26 / 28 | 14 |
| `5C.PBS11` | between Yanchep and Muchea | 12–43 dB | 21 / 30 | 12 |
| `5C.PBS26` | Kwinana (32.22° S) | 9–24 dB | 8 / 8 | 5 |
| `5C.PBS16` | north of Regans Ford | 7–21 dB | 11 / 26 | 5 |
| `5C.PBS20` | Wanneroo | to 27 dB | 5 / 17 | 4 |
| `5C.PBS37`, `5C.PBS12`, `5C.PBS22` | central Perth, Two Rocks, east of Regans Ford | to 15 dB | 2–6 | 1–2 |

The 37 agreeing pairs have a median r = 0.98 and differ by 1.8 mHz RMS after 60 s
smoothing, up to 136 km apart (Regans Ford–Kwinana, 2017-02-15: r = 0.93). Every month
from 2017-01-25 to 2017-06-15 has windows with an agreeing pair. The second deployment
(2017-07..11, other sites) has no humming station. As for IPS/UPS, no station is
believed alone: a second site must agree for a second to be stored.

### Raspberry Shakes: screening

30 minutes from 04:45 UTC on the 15th of every second month, 2018-02..2026-10, for every
Shake whose registered position lies in Western Australia or the Northern Territory
(53 dates; 297 of 464 requests returned data). A station's score for a year is the
share of its probes in which the decoder returned a track for more than half of the
seconds. This is stricter than the China lists' "line at 7 dB": the decoder also demands
that the roll-off model wins, which a half hour does not always settle (2023-06-15: lines
of 7–23 dB at four stations, nothing kept).

**South West (16 stations kept).** From 2022-08 three to eight stations decode in almost
every probe, between Toodyay, the Perth suburbs, Mandurah and Harvey (170 km end to
end). Before that: `AM.RBA34` from 2022-02, and `AM.RBA56` in one probe of 2020.
`AM.RED42` (2018-09 on) never decodes: its strongest line is a motor's (below).

Several units share an owner and a rounded position (three near Midland, four near
Harvey), so stations within a kilometre count as one site: they must not confirm each
other.

**Darwin (3 stations, one address at a time).** `AM.RBA56` decoded in 1 of 5 probes in
2018, none in 2019, 5 of 8 in 2021-04..2022-06. `AM.R9544` decodes in about half of the
probes from 2024-10; `AM.RFE60`, at the same address, from 2026-08. Nothing in between
(2022-08..2024-08).

`AM.RBA56` was registered in Darwin, then in central Perth (2019-12-08..2021-03-18),
then in Darwin again. A month is fetched whole, so its epochs in the two lists are cut
to whole months on one side. Both moves are borne out by the data: on 2020-01-10 it
shows the South West's load-shedding event (below), and in 2022-04 and 2022-06 it is
uncorrelated with Perth's stations (r = −0.36 to −0.06).

### Validation of the Shake decodes

**Against a direct line.** `2P.SWN22` (Lake King, 380 km east-south-east of Perth;
250 samples/s, GPS-timed) carried the grid's line on two probe dates, at 12 and 18 dB.
Three Shakes in Perth, each decoded alone, against it (30 minutes from 04:45 UTC, 12 s
smoothing; the direct line's own noise at 12 dB is about 10 mHz):

| Date | r | RMS difference |
|---|---|---|
| 2022-12-15 | +0.81, +0.86, +0.87 | 15–19 mHz |
| 2023-02-15, where the decoder now answers | +0.88, +0.89, +0.91 | 11–15 mHz |

Minute by minute on 2022-12-15, direct line then `AM.R11DC`: −27/−25, −11/−10, +7/+6,
… −59/−55, −65/−58, −19/−20 mHz. The sign is the grid's on both dates, in both
directions, without being told.

**Beyond the decoder's range.** In the last ten minutes of the 2023-02-15 window the
grid ran up to +130 mHz. The decoder, whose states end at ±80 mHz, answered with values
near zero where it answered at all (`AM.RBA34`: 0 to +4 mHz for seven minutes against
+85 to +130), and since the stations agreed with each other the fusion would have kept
them. `track_nyquist` now looks for
the beat itself in 64 s spectra: where its strongest line lies beyond 60 mHz, those
seconds and half a minute either side are left empty. With that, the three stations
keep 59–66% of the half hour and stand at r = 0.88–0.91 against the direct line over
what they keep (before: −0.02, 0.30 and 0.88, with 308 and 575 seconds more than
40 mHz off at two of them).

**Between sites.** Two hours from 2026-06-15 04:00 UTC, each station decoded alone,
compared after 12 s smoothing (the grid's standard deviation: 25 mHz):

| Stations | r | RMS difference |
|---|---|---|
| Seven South West stations, 21 pairs (lines of 11–20 dB) | +0.84 to +0.94 | 8–14 mHz |
| The eighth (`AM.RBB6B`, 10 dB) against the seven | +0.65 to +0.74 | 16–19 mHz |
| Darwin (`AM.RFE60`) against the seven | −0.27 to −0.38 | 37–42 mHz |

Over all probes: 247 pairs of South West stations at different sites, median r = 0.90,
91% at 0.7 or more. Two thirds of the pairs below 0.7 fall on three dates (2023-04-15,
2024-06-15, 2025-04-15), when most pairs disagree at once: the grid moved little
(15–21 mHz of spread) or a stretch came out mirrored at some stations. The two Darwin units at one
address agree at r = 0.97–0.99, which tests the decoder's repeatability and nothing
else. Perth and Darwin never agree (r = −0.36 to +0.15 in 39 pairs).

**Against a published event: 10 January 2020.** AEMO's investigation report on the
South West's under-frequency load shedding gives a timeline to the second
([AEMO, June 2020](https://www.aemo.com.au/-/media/files/electricity/wem/security_and_reliability/2020/aemo-investigation-report---swis-ufls-event-10-january-2020.pdf)).
An excursion of 1.25 Hz is far outside the decoder's ±80 mHz, but it can be read
straight off the spectrum of `AM.RBA56` (then in Perth) and `AM.RED42`, in 30 s windows:

| AEMO (AWST = UTC + 8 h) | Shakes (UTC) |
|---|---|
| 20:08:30 NewGen Kwinana trips; nadir 49.26 Hz | the strongest line of both stations jumps by 0.6–0.7 Hz between the windows centred on 12:08:27 and 12:08:42; the mains line appears at 49.31 Hz (12:09:27) |
| 20:10:23 frequency falls below 49.00 Hz | mains line at 49.19 Hz (12:10:12), 49.02 Hz (12:10:27), 49.00 Hz (12:10:42) |
| 20:12:08 load shedding at 48.748 Hz | mains line at 48.80 Hz in the window centred on 12:11:57, the lowest read |
| 20:17:38 back to 49.8 Hz | both stations' lines are within 0.1–0.2 Hz of where they started by 12:17:40–12:18:10 |

This fixes three things at once: these stations were on the South West grid, their
clocks are right to the second, and the line grows as the frequency falls (the mains
line in `AM.RBA56`: below the noise at 50 Hz, amplitude 2.1 at 49.31 Hz, 3.9 at
49.00 Hz, 5.5 at 48.80 Hz), which is the sign of the roll-off the decoder assumes.

**Motor lines.** The strongest line in both stations that evening is not the mains but
1.19 Hz (`AM.RBA56`) and 0.63 Hz (`AM.RED42`) below it, following it step for step: a
two-pole induction motor nearby (a pump, a compressor), turning at the grid's frequency
less its slip of 2.4% and 1.3%. A geophone feels it mechanically. Such a line tracks
the grid but its offset moves with the motor's load, so it is no reference; the decoder
never sees it (it looks within ±80 mHz of 50 Hz), which is why `AM.RED42` scores zero
in eight years.

**Through the downloader** (one month each):

| Source, month | Seconds stored | Stations (share of the month each contributed) |
|---|---|---|
| `seismic_perth_basin` 2017-03 | 71.6% (92% of them with two or more sites) | `PBS11` 54%, `PBS15` 53%, `PBS20` 28%, `PBS26` 15%, `PBS12` 15%, `PBS16` 7%; 4.9 GB |
| `seismic_swis` 2026-06 | 96.0% (96% of them with two or more sites) | `R1E83` 83%, `RBA34` 79%, `R4477` 78%, `R1BCE` 73%, `R7100` 58%; 1.8 GB |
| `seismic_darwin` 2026-08 | 23.2% | `R9544` 22%, `RFE60` 14% |

The Darwin month was made while its two units, which share an address, still counted
as two sites; they now count as one, so a later fetch keeps only seconds whose line is
strong enough to stand alone (13 dB) and will store less (the backfill below stored
2026-08 at 26%).

**The backfill** (stored and in the bucket, 2026-11-12; share of each month's seconds
stored, mean of the months stored):

| Year | `seismic_swis` → `australia_swis` | `seismic_darwin` → `darwin_katherine` |
|---|---|---|
| 2018 | – | 3 months (10..12), 0.9% |
| 2020 | 12 months, 15.1% (one station) | – |
| 2021 | 1 month, 41.9% (one station) | 9 months (04..12), 6.9% |
| 2022 | 12 months, 85.8% | 7 months (01..07), 9.4% |
| 2023 | 12 months, 89.3% | – |
| 2024 | 12 months, 94.5% | 5 months (08..12), 19.8% |
| 2025 | 12 months, 90.8% | 11 months (all but 10), 28.6% |
| 2026 | 10 months (01..10), 93.2% | 8 months (01..04, 07..10), 23.9% |
| All | 71 months, 77.2% | 43 months, 17.1% |

2023-03..2026-10 for the South West: 84.6–97.3% each, 91.9% on average. Darwin's
months not listed failed: 50 with no screened station online (2019-01..2021-03,
2022-08..2024-07), and 2022-08, 2025-10, 2026-05 and 2026-06 with data but no second
whose hum passed the checks. None failed for downloads.

**End to end.** A 12-minute synthetic clip whose 50 Hz tone (with harmonics and noise)
follows `AM.R1E83` alone on 2026-06-15 from 04:40:00 UTC, decoded by a separate tool,
is located in the stored 2026-06 at 04:39:59 UTC, STRONG, r = 0.927, 3.1 mHz RMS; the
best wrong alignment scores 14.0 against 46.4. That station is one of the five behind
the stored month, so this tests the store and the search, not the decoder.

**Darwin is not checked against anything**: no recorder, no direct line and no second
site exist there. Its sign rests on the roll-off measured in Germany and confirmed in
Perth.

### Limits

For the Shakes, those of the China sources ([#china-seismic_cn_](#limits)):
a stretch of tens of minutes now and then mirrored at every station alike, and coverage
that follows the hobbyists. In addition:

- **Accuracy.** 11–19 mHz RMS per station against a direct line on the two dates above,
  on a grid that moves 20–30 mHz: coarser than the 4–7 mHz measured in Germany.
- **Range.** The South West leaves ±60 mHz for minutes at a time (ten minutes of the
  2023-02-15 half hour), and those seconds are empty. A recording made during a large excursion
  finds nothing to match.
- **2017 is a direct line** (`seismic_perth_basin`): 1–2 mHz, GPS-timed, no sign to
  recover; none of the above applies to it.

- **Darwin is one address.** Nobody confirms it; where it was is its owner's word.
  The Darwin–Katherine system is small and moves more than a large grid
  (11–24 mHz of spread in the probes, with excursions beyond the decoder's range).
- **The South West in 2020–2021 is one station** (`AM.RBA56`, then `AM.RBA34`), and
  it decoded in few probes.
- **Gaps.** The South West has nothing before 2017-01-25, nothing from 2017-07 to
  2019-12 and little until 2022-02; Darwin nothing before 2018-10, in
  2019-01..2021-03 or in 2022-08..2024-07.
- **Pilbara, Mount Isa, Alice Springs: no station.** No Shake is registered in any of
  them, and the school seismometers there filter the line out. Horizon Power's town
  systems (Broome, Kununurra, Esperance, Carnarvon, ...) are not even outlined.

### Producing the data

As for China: never on the dashboard (`GEOLOC_SHAKE_REFRESH` and `GEOLOC_AUSPASS_REFRESH`
are not set there), backfilled offline and pushed. The Shake archive sets the pace, not the CPU: it
streams ~0.5 MB/s per request and answers 429 past about one request every 1.5–2 s, so
each station-day is one request, ten in flight. A South West month takes 10–16 minutes,
a Darwin month with data 2–4; the 2023-03..2026-10 backfill took about 9 h. The 2017
array costs about 1 GB of download per station and month.

    GEOLOC_SHAKE_REFRESH=1 GEOLOC_AUSPASS_REFRESH=1 geoloc refs refresh --grid australia_swis
    GEOLOC_SHAKE_REFRESH=1 geoloc refs refresh --grid darwin_katherine
    geoloc refs push

## Brazil (`seismic_brazil`)

Code: the tracker and fusion of [`seismic_east`](#us-eastern-seismic_east)
(`src/refs/seismic.cpp`, `src/refs/seismic_fetch.cpp`); stations in
`src/refs/seismic_brazil_stations.inc`.

No operator or research group publishes per-second frequency for Brazil's Sistema
Interligado Nacional (SIN, 60 Hz). ONS's open-data portal holds frequency quality
indicators only. `seismic_brazil` reconstructs 2004-02..2008-08 from the mains hum in
old short-period seismic recordings archived by the University of São Paulo.

### Where the hum is

The University of São Paulo's FDSN service (`seisrequest.iag.usp.br`, network `BL`)
archives a group of 1 Hz short-period stations (`CAM1`, `CAM3`, `CAM4`, `VAO`, `VAO2`,
`VAO3`; S-13 sensors, 100 samples/s `EHZ`) continuously from 2002-09 to 2008-08. Their
channels come from one recorder: segments start and end together on all of them.

At 100 samples/s the 60 Hz line folds to 40 Hz. The recorder's anti-alias filter lets
it through:

| Channel | Hum (12 s frames) | What the channel is |
|---|---|---|
| `BL.CAM1..EHZ` | ~45 dB | An input with no live sensor: a few counts rms, no earthquake arrivals of its own |
| `BL.VAO3..EHZ` | ~45 dB | The same signal as `CAM1` (waveforms correlate at r 0.94–1.00) |
| `BL.CAM4..EHZ` | 10–28 dB | A working seismometer: thousands of counts, clear P arrivals |
| `BL.VAO`, `VAO2`, `CAM3` | none | — |

`CAM1` and `VAO3` are therefore one witness, and `VAO3` is not used. `CAM4` is a
different input with a real sensor on it; its weaker hum is the only check on `CAM1`.
The hum is picked up at the recorder, not at the sensors' sites 470 km apart, so the
whole source is one place on the grid.

### The stray tone, and the median noise floor

Every channel also carries a fixed tone at 40.65 Hz, about 5 dB stronger than the hum.
It is outside the tracker's ±0.25 Hz search, so the frequency track is unaffected, but
it sits among the noise probes (0.6–1.0 Hz from the line). With the usual mean of the
probes the SNR reads 2–3 dB and fusion rejects every second.

`TrackOptions::robust_floor` (set for this network only) takes the median of the 24
probe powers, divided by ln 2 (noise power is exponentially distributed, so its median
is ln 2 of its mean). A tone covering fewer than half the probes then does not count
as noise. `seismic_east` and `seismic_ips` keep the mean and their stored values.

### Verification

With the project's `track_hum`, one-hour windows at 15:00 UTC:

| Date | r(CAM1, VAO3) | r(CAM1, CAM4) | rms CAM1−CAM4 |
|---|---|---|---|
| 2003-11-05 | 0.34 | 0.70 | 14.9 mHz |
| 2004-02-05 | 0.999 | 0.60 | 22.2 mHz |
| 2004-06-05 | 1.000 | 0.996 | 1.9 mHz |
| 2005-05-05 | 1.000 | 0.996 | 1.1 mHz |
| 2006-07-05 | 1.000 | 0.995 | 1.4 mHz |
| 2007-01-05 | 1.000 | 0.996 | 2.5 mHz |
| 2007-09-05 | 0.998 | 0.71 | 14.1 mHz |
| 2008-08-25 | 1.000 | 0.63 | 21.4 mHz |

The line wanders by 10–20 mHz around 60.000 Hz, as a grid does and an equipment tone
does not. Before 2004-02 the hum is weak or absent (the archive starts 2002-09).
Nothing is archived after 2008-08. Roughly one sampled day in five has no data.

### Timing

The recorder is not known to be GPS-disciplined. Its clock was checked against the P
arrivals of two earthquakes on `CAM4`, the live channel, using iasp91 travel times:

| Earthquake | Predicted P | Observed onset | Difference |
|---|---|---|---|
| 2005-06-13 Tarapacá M7.8 | 22:49:46.1 | 22:49:44.5 | −1.6 s |
| 2007-11-14 Tocopilla M7.7 | 15:46:12.7 | 15:46:13.7 | +1.0 s |

Both are within the travel-time model's own error at 25°, so the clock is within about
2 s of UTC at those two dates. 2004 and 2008 are unchecked: no data for the 2004 event
tried, and the 2007-08-15 Pisco arrival was too weak to pick.

### How a month is built

Both channels are fetched for the whole month (6 h requests, no account needed).
Where `CAM4` agrees with `CAM1` (r ≥ 0.9 in 10-minute windows) the two are fused;
elsewhere `CAM1` stands alone if its noise estimate is ≤ 1.5 mHz (SNR ≥ 28.5 dB). The
manifest records both counts (`seconds_multi_station`, `seconds_single_station`).

### A fetched month

2005-05, fetched end to end (1.08 GB downloaded: 0.27 GB for `CAM1`, 0.81 GB for `CAM4`):
90.5 % of the month's seconds stored. Of those, 2,244,060 s (92.5 %) rest on both
channels agreeing and 180,764 s (7.5 %) on `CAM1` alone. The missing 9.5 % are gaps in
the archive.

### Limits

- One recorder, one place. A second independent site was not found.
- When `CAM4` is weak, a value rests on `CAM1` alone. It was confirmed by `CAM4` in
  every year from 2004 to 2007, but not in each month.
- Absolute time is good to about 2 s, not to the millisecond as with GPS-timed stations.
- No independent frequency recording overlaps 2004–2008 to compare against.
- The source ranks below measured recorders (rank 60), should any appear.

### What else was screened in South America (2026-11)

Nothing usable: Colombia `CM` (200 samples/s accelerometers and 100 samples/s
short-period, 2018–2026), northern Chile `IQ`, `8F`, `5E`, `8G` (2009–2018), central
Chile `ZE`, `XY` (2010), `C1.LR02`, Argentina `XN` (2022). Ecuador's nodal array `2N`
(EarthScope, 2022-03-10..04-15, 250 samples/s) shows a 60 Hz line agreeing between
nodes at r 0.5–0.84 on a quick tracker; it was not confirmed with `track_hum`.

### Screening for 2008-09 to today (2026-11)

The aim was to extend the reference past 2008-08. Nothing found carries the grid
continuously. Each source below was probed with `track_hum` (60 Hz), 30 minutes from
15:00 UTC. A real line shows as near-full coverage at 9 dB or more; noise reads 3–5 dB
at about 80 % coverage.

- **USP's own archive** (`seisrequest.iag.usp.br`; networks BL, BR, VL, XC, 7F, XV;
  every channel ≥ 100 samples/s). Probed on 2009-06, 2011-06, 2012-10, 2014-06, 2018-06,
  2023-06 and 2026-06, with 30–60 channels per date. No station hums at 40 Hz. The
  200 samples/s channels (`MCI6`–`MCI9`, `BB22B`, `LDASE`, `PRB1`), where 60 Hz is in
  band, reach 4–7 dB. The best single readings, `BR.SDBA` (2023-06) and `BL.RCLB`
  (2012-10), have 7–10 dB on one date only. Modern digitisers filter the line out
  before decimation. `CAM1` was an exception because of its old recorder.
- **EarthScope** (RSBR network `ON`, 19 stations 2011 on; `BL.RCLB`, `VABB`, `PMNB`;
  `GT.BDFB`; `IU.RCBR`, `SAML`, `PTGA`). The 100 samples/s channels show nothing
  (3.5–5.8 dB on 2015-06-15). The 200/500 samples/s Bebedouro array (`BEB*`, `BB1*B`,
  2005–2010) is event-triggered, with 0–12 % of a half hour archived.
- **Raspberry Shakes** (network `AM`). 61 units are registered inside the SIN outline.
  Probes ran every second month, 2017-02..2026-10 (46 dates). Only 1–11 of them per
  date have archived data. One unit hums at 40 Hz: `AM.REDD8` (Santa Catarina, online
  since 2026-09-29), at 11–19 dB on four of six probes in 2026-10 and none on
  2026-11-05. Its track wanders like a grid (±30 mHz over minutes), but there is no
  second site to confirm it. The 50 samples/s units (`RF442`, `R2DD9`), where 60 Hz
  folds to 10 Hz, show only seismic noise. `RD272` and `SB293` carry a line at 20 Hz
  (where 120 Hz folds). It sits fixed at 0 mHz and the two units do not correlate
  (r = −0.02), so it is an internal tone.
- **Measured series.** None was found. ONS publishes frequency quality indicators
  only. MedFasee (UFSC) shares PMU data on request. Zenodo, figshare, Mendeley and IEEE
  DataPort hold Brazilian power data at 15 min or hourly resolution, or switching
  transients, but no frequency log.

The SIN after 2008-08 needs one of these: a PMU archive obtained on request (MedFasee,
ONS), or Raspberry Shakes humming at two sites at once. `AM.REDD8` can become a
one-station source like `seismic_darwin` if another Shake in the South starts to hum.
