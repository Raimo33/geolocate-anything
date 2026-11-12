# Western Australia and Darwin from seismometer hum (`seismic_swis`, `seismic_perth_basin`, `seismic_darwin`)

Code: the same as [`seismic_cn_*`](seismic_china.md) for the Raspberry Shakes
(`track_nyquist` in `src/refs/seismic.cpp`) and as [`seismic_east`](seismic_east.md) for
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

## What was searched (2026-11-10)

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

## The Perth Basin Array, 2017 (`seismic_perth_basin`)

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

## Raspberry Shakes: screening

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

## Validation of the Shake decodes

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

## Limits

For the Shakes, those of the China sources ([seismic_china.md](seismic_china.md#limits)):
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

## Producing the data

As for China: never on the dashboard (`GEOLOC_SHAKE_REFRESH` and `GEOLOC_AUSPASS_REFRESH`
are not set there), backfilled offline and pushed. The Shake archive sets the pace, not the CPU: it
streams ~0.5 MB/s per request and answers 429 past about one request every 1.5–2 s, so
each station-day is one request, ten in flight. A South West month takes 10–16 minutes,
a Darwin month with data 2–4; the 2023-03..2026-10 backfill took about 9 h. The 2017
array costs about 1 GB of download per station and month.

    GEOLOC_SHAKE_REFRESH=1 GEOLOC_AUSPASS_REFRESH=1 geoloc refs refresh --grid australia_swis
    GEOLOC_SHAKE_REFRESH=1 geoloc refs refresh --grid darwin_katherine
    geoloc refs push
