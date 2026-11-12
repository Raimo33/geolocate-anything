# US Eastern grid frequency from seismometer hum (`seismic_east`)

Code: `src/refs/seismic.cpp` (tracking and fusion), `src/refs/seismic_fetch.cpp`
(EarthScope downloads), `src/refs/mseed.cpp` (miniSEED reader),
`src/refs/seismic_stations.inc` (screened station list). How it fits the reference store:
[architecture.md](architecture.md#ingestion).

No operator publishes per-second frequency for the US Eastern Interconnection, and the
FNET/GridEye archive is available only on request. `seismic_east` reconstructs it from
the 60 Hz mains hum that seismometers and accelerometers pick up inadvertently.

## Rationale

A sensor or digitiser installed in a building or powered from the mains
couples to the 60 Hz field electrically or mechanically. The digitisers are GPS
disciplined, so every sample is timed to the millisecond, and FDSN data centres
(EarthScope) archive the waveforms continuously and openly. Each such station is
therefore an unintended GPS-timed grid frequency recorder.

## Channel selection and aliasing

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

## Screening

All candidate channels (858 at ≥ 200 samples/s, plus 100 samples/s channels
before 2014) were tracked over 15-minute windows sampled each year (quarterly before 2014).
Tracks were smoothed over 12 s and correlated pairwise. A channel is retained if it
reached r ≥ 0.97 with another channel at least once. Agreement is the criterion because
the grid frequency is the only signal all stations share, whereas a motor or power-supply
tone drifts independently. The resulting list holds 67 channels at 55 sites in 13
networks.

## Station choice per month

Each station is scored by its correlation in the nearest
screened year (penalised by 0.9 if more than 3 years away). Up to five stations with a
score ≥ 0.9 are chosen, at most one channel per site. The best three are fetched for the
whole month; the other two are fetched only for 6-hour chunks in which fewer than three
stations supported the fused value.

## Tracking a station

For each contiguous segment:
1. The hum line is mixed to 0 Hz, low-pass filtered (Kaiser FIR, flat to ±1 Hz, −80 dB
   from 3 Hz) and decimated to 10 samples/s in one zero-delay complex FIR.
2. For each UTC second, a 12 s sin²-windowed frame is searched at 5 mHz steps within
   ±0.25 Hz of nominal; the peak is refined by parabolic interpolation. Peaks on the edge
   of the range are rejected.
3. SNR is the peak power against 24 noise probes at 0.6–1.0 Hz on either side.

The 12 s frame matches the frame used on query audio, so reference and query are smoothed
identically.

## Fusing stations

1. A station-second is valid if its SNR is ≥ 8 dB and it lies within ±400 mHz of nominal.
2. Over 10-minute windows (5-minute hop), each station's noise is estimated as
   40 mHz / √SNR, following the 1/√SNR law for a tone's frequency error.
3. Stations whose smoothed tracks correlate at r ≥ 0.9 agree. Stations agreeing with at
   least half as many others as the best-connected station are accepted.
4. If no two stations agree, only the quietest may stand alone, and only if its noise is
   ≤ 1.5 mHz; this guards against equipment tones.
5. Per second, the inverse-variance weighted mean of accepted stations is stored, after
   rejecting outliers farther than max(5 mHz, 4σ) from the weighted median.

## Validation

- *Independent recorder.* Against KIT's recorder in Florida (2023-06/07), single stations
  reach r = 0.997–0.9997 (0.4–1.1 mHz RMS) and the fused series r = 0.996–0.9993
  (0.6–0.9 mHz RMS per day). Four seismic stations in different states and networks agree
  with each other at zero lag, while KIT's lag drifts from −4 s to −13 s; the drifting
  clock is therefore KIT's.
- *Pre-2014 stations.* On 2005-07-15, three `LD` stations at 100 samples/s tracked 95% of
  seconds at median SNRs of 26–38 dB and agreed within about 2 mHz.

## Coverage

Share of seconds with a value, 2026-11-09:

| Years | Coverage | Weak months |
|---|---|---|
| 2001 (09–12) | 76% | 2001-09: 41% |
| 2002–2004 | 90–96% | 2002-01..05: 71–88%; 2003-09: 41% |
| 2005–2007 | 97–99% | — |
| 2008–2009 | 92–97% | 2008-05: 59%; 2009-10: 87% |
| 2010 | 88% (8 months) | 2010-01..04: stored as no data, 2010-05: 17% (station choice; [rescreen](#rescreen-of-20102013-2026-11-12)) |
| 2011 | 90% | 2011-12: 23% |
| 2012 | 1% | 2012-01: 19%; 2012-02..12: stored as no data (station choice; [rescreen](#rescreen-of-20102013-2026-11-12)) |
| 2013 | 41% | 2013-03, 05..11: 0–49% (station choice; [rescreen](#rescreen-of-20102013-2026-11-12)) |
| 2014–2015 | 88–96% | 2014-08: 71%; 2015-05..08: 60–78% |
| 2016–2017 | 90–93% | 2016-02/08/09: 62–69%; 2017-10: 35% (on 2017-10-15 over a dozen listed sites agree at r ≥ 0.98: refetch) |
| 2018–2026 | 96–100% | 2021-06/10/11: 70–83% |

## Rescreen of 2010–2013 (2026-11-12)

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
probes), and some evening probes of 2011-12..2012-02 (PAL alone). The months are refilled
with `geoloc refs fetch seismic_east --grid us_eastern --months 2010-01:2013-12 --force`.

The same weakness shows elsewhere: the all-channel screen of 2015-06-15 (a 60–78% month)
found 30-odd Eastern channels humming, many unlisted (GS.KAN05/06/08/13/17 in Kansas,
YC.LA10 in Louisiana, NQ.NLKCP, horizontals of KY.HEKY), and 2017-10-15 (35%) over a dozen
listed sites at r ≥ 0.98 (GS.DEC08/09/17, NP.7405, N4.T45B, KY.HZKY, PE.ALL05, NQ.RSTON).
Screening every month, not once a year, would likely lift the other weak months too.

## Negative results

- *Harmonics.* The 120 and 180 Hz harmonics are absent (0–5 dB, against 29–40 dB at 60 Hz),
  including on a 500 samples/s channel, so they cannot fill gaps. Remaining gaps come from
  the lone-station rule.
- *Other regions.* The same screening found no usable network in China, Brazil, Western
  Australia, Québec or Taiwan, nor among Raspberry Shakes or modern 100 samples/s networks
  (including Japan's Hi-net and K-NET). The method needs continuously archived channels
  that see the line at sites far enough apart to confirm each other. Besides the eastern
  US, IPS/UPS has them since 2022: [seismic_ips.md](seismic_ips.md). In China the
  Raspberry Shakes do carry the 50 Hz line, on the Nyquist frequency of their
  100 samples/s, where this tracker cannot read it; a separate decoder does:
  [seismic_china.md](seismic_china.md).
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

## Open questions

How the hum couples into each instrument; why a station's agreement
varies from year to year; accuracy against a PMU archive (FNET/GridEye) over a longer
period; whether older networks extend coverage before 2001 (1975-01..1984-07 were fetched and nothing passed; 1984-08..2000 was not tried, since no listed station existed then); whether monthly screening lifts the remaining weak months (2015, 2016, 2017-10, 2021).
