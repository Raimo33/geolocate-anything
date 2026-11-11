# IPS/UPS grid frequency from seismometer hum (`seismic_ips`)

Code: the same as [`seismic_east`](seismic_east.md) (`src/refs/seismic.cpp`,
`src/refs/seismic_fetch.cpp`); the screened stations are in
`src/refs/seismic_ips_stations.inc`.

No operator of the IPS/UPS synchronous area (Russia, Belarus, Kazakhstan, Central Asia,
the South Caucasus; Ukraine and Moldova until 2022-03-16, the Baltic states until
2025-02-08) publishes per-second frequency. The only open measurements are KIT's
campaigns: St. Petersburg and Tallinn in 2019-03..05 (`osf_edr`) and Kaunas and Tallinn
in 2025-01-23..02-08 (`osf_baltic`). `seismic_ips` reconstructs the frequency from
2022-10 on from the 50 Hz hum in strong-motion recordings archived at EarthScope, the
way `seismic_east` does for the US Eastern Interconnection.

## What differs from the Eastern Interconnection

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

## Screening (2026-11-09)

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

## Coverage

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

## Validation

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

## Negative results

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

## Ukraine

Ukraine and Moldova have been part of Continental Europe since 2022-03-16, so a
recording from government-held Ukraine after that date is matched against the
Continental European references, which are continuous. What stays uncovered:

- Ukraine before 2022-02-24 (IPS/UPS; only KIT's 2019-03..05 campaign), and the island
  operation of 2022-02-24..03-16.
- Crimea since 2015 and other areas reconnected to the Russian system: IPS/UPS, covered
  by `seismic_ips` from 2022-10 where two stations agree.
- EarthScope holds two stations in Ukraine at a usable rate (`IU.KIEV`, 40 and 80
  samples/s until 2017): no line.

## Open questions

- European data centres (GEOFON and the other EIDA nodes) archive the Baltic, Ukrainian,
  Moldovan, Georgian and Romanian networks and were not screened: they were not
  reachable from the development sandbox. Stations there in the Baltic states before
  2025-02 and in Ukraine and Moldova before 2022-03 could extend IPS/UPS back in time,
  if any records at 200 samples/s or more.
- Elering, AST and Litgrid measured IPS/UPS frequency until 2025-02-08; whether any of
  them serves an archive at seconds resolution was not checked (same reason).
- Whether the months with a single humming station can be used with a plausibility test
  on that station (spread and mean of the track) instead of a second site.
