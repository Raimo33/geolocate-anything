# geolocate-anything

An OSINT tool that estimates **where and when an audio or video recording was made** from
background sounds that people rarely notice. Four signals are planned:

1. [**Grid hum**](#1-grid-hum-enf): the mains frequency gives the power grid and the exact UTC second.
2. [**Aircraft**](#2-aircraft): Doppler curves of passing propeller aircraft, matched against
   ADS-B history, give the place to within a few hundred metres and the start time.
3. [**Church bells**](#3-church-bells)
4. [**Emergency sirens**](#4-emergency-sirens)

The grid hum and aircraft are implemented, and each feeds the other: the hum's second
lets one flyover locate, and the aircraft's place and time let a faint hum confirm.

---

## 1. Grid hum (ENF)

Documentation:
- [`docs/grids/architecture.md`](docs/grids/architecture.md): design and evaluation
- [`docs/grids/datasets.md`](docs/grids/datasets.md): grid data sources
- [`docs/grids/compression.md`](docs/grids/compression.md): reference store format
- [`docs/grids/seismic.md`](docs/grids/seismic.md): grid frequency from seismometer hum (US Eastern, US Western, ERCOT, IPS/UPS, China, Western Australia and Darwin, Brazil)
- [`docs/metadata/architecture.md`](docs/metadata/architecture.md): time zone from the metadata clock (step 7)
- [`docs/metadata/datasets.md`](docs/metadata/datasets.md): time-zone data for the metadata clue (step 7)

### Principle

Mains electricity runs at a nominal 50 or 60 Hz, but the actual frequency (the *Electrical
Network Frequency*, ENF) wanders by a few millihertz from second to second as supply and
demand shift. Every generator in a synchronous grid turns at the same rate, so this wander
is identical everywhere on a grid and different between grids. Microphones near mains
wiring or appliances pick up the hum. Matching the hum's frequency trace against logged
grid frequency identifies **the grid** (continent scale) and **the UTC second the
recording started**.

### Method

1. **Metadata.** GPS tags, capture timestamps and the device model are read from the
   container. They are cross-checked against the result in step 7.
2. **Hum detection.** The nominal frequency (50 or 60 Hz) is detected, which already
   splits the world in two.
3. **Edit detection.** Edited videos join clips recorded at different times. The file is
   cut wherever the hum's phase jumps, and each segment is analysed independently.
4. **ENF extraction.**
   - Harmonics are selected by mutual *agreement* rather than loudness. Device tones
     (chargers, fans, coil whine) near mains harmonics are often louder than the hum,
     but only true mains harmonics move together.
   - Each harmonic is evaluated by exact DFT on a shared frequency grid and normalised by
     its own noise floor. The harmonics are combined as a generalised likelihood ratio.
   - Viterbi decoding finds the most plausible smooth path, since grid frequency cannot
     jump.
5. **Search.** Every stored second of every candidate grid is streamed from memory-mapped
   files and aligned by overlap-save FFT cross-correlation (about 19 million candidate
   alignments per second per core, blocks spread over every core). Each alignment is scored on the shape of the
   wander and on its absolute level, allowing for the recorder's clock error.
6. **Verdict.** The scores of the many wrong alignments form an extreme-value null
   distribution, which gives the probability that a wrong alignment would lead by the
   observed margin. This maps to STRONG / MODERATE / WEAK / NONE.
7. **Time zone.** Because the hum gives true UTC, a metadata clock that differs from it by
   a real UTC offset reveals the device's time zone. Only the parts of the matched grid
   that observed that offset at that instant (DST included) remain.

### Reference data

Each grid is backed by one or more sources. Where sources overlap they were cross-checked
second by second, which is how most of the defects below were found.
All series are stored at 1 s resolution, rounded to 1 mHz.

| Grid | Source | Period | Description and construction | Defects and corrections |
|---|---|---|---|---|
| Great Britain | `neso` | 2014 → last month | NESO open data, 1 s. | 2014 files use day-first dates in UK local time; 2025-04..10 files are stamped in BST for the whole month. Both converted to UTC. Values are in mHz with a misleading median; rescaled. |
| Great Britain | `osf_pgf` | 2014–2020 | OSF power-grid-frequency collection; verified identical to NESO. | As `neso`. |
| Great Britain | `pmu_zenodo` | 2025-04-28 → 2025-12 | PMU in London, 1 s, UTC. | — |
| Continental Europe | `osf_pgf` | 2011-07 → 2020-09 | Germany at 1 s; gaps filled from France at 10 s. | Germany is in German wall-clock time (converted to UTC). France swaps day and month for days 1–12 and runs 17 s early against Germany; both repaired before gap filling. |
| Continental Europe | `open_enf` | 2010, 2011-02..06, 2012-01 | Open-ENF German database (2010: 10 mHz steps); German TSOs' 4 s series for 2011–12. | 2010 recorder had no GPS time: timing only good to about ±2 s. |
| Continental Europe | `netztransparenz` | 2012-02 → 2022-06 | German TSOs, 1 s; 2022-03 from TransnetBW's monthly file. | German wall-clock time and 2 s late against GPS-timed recorders; both corrected. |
| Continental Europe | `energy_charts` | 2022-05 → today | Fraunhofer ISE, Freiburg, 1 s, UTC. | — |
| Nordic | `fingrid` | 2015 → last month | Fingrid open data, 10 Hz averaged to 1 s. | Published in Finnish local time, not UTC; the autumn DST day logs one hour twice. Converted to UTC. |
| Nordic | `osf_pgf` | 2015–2020 | The same Fingrid files via OSF. | As `fingrid`. |
| Ireland | `osf_ireland` | 2014-05 → 2017 (gaps) | GPS-timed recorder near Dublin, 10 Hz averaged to 1 s. | — |
| Ireland | `kit_zenodo` | 2021-12 | KIT/Jülich recorder release on Zenodo. | — |
| Ireland | `eirgrid` | mid-2020 → today | EirGrid Smart Grid Dashboard, 5 s at 10 mHz. | Irish wall-clock time and 2 s late; corrected. |
| Australia (NEM, Tasmania) | `aemo_causer_pays` | 2011-01 → 2017-03 | AEMO FCAS data, 4 s. | — |
| Australia (NEM, Tasmania) | `aemo_annual` | 2010–2025 | AEMO yearly frequency workbooks, 4 s, imported with `refs import-aemo`. | — |
| Australia (NEM, Tasmania) | `aemo_fpp` | 2024-12 → today | AEMO regional frequency, 4 s; mainland from NSW, gaps from QLD. | — |
| Western Australia (SWIS) | `seismic_perth_basin` | 2017-01-25 → 2017-06 | Reconstructed from the 50 Hz hum seen directly in the Perth Basin Array (250 samples/s, GPS-timed, at AusPass); only seconds on which two sites agree. See [below](#western-australia-and-darwin-from-seismometer-hum). | A temporary deployment: nothing after June. |
| Western Australia (SWIS) | `seismic_swis` | 2022-02 → today; one station in 2020 | Reconstructed from the 50 Hz hum in Raspberry Shake geophones between Toodyay, Perth and Harvey, decoded as for China. | 11–19 mHz per station against a direct line; seconds beyond ±60 mHz are empty. Not GPS-timed (NTP). |
| Darwin–Katherine | `seismic_darwin` | spells of 2018-10 → 2022-07, 2024-10 → today | Same method: one address in Darwin. | One station: nothing to confirm it. |
| US Western | `seismic_west` | 2001-09 → 2018-12, 2025-08 → today | Reconstructed from the 60 Hz hum in GPS-timed seismometers from Vancouver Island to New Mexico; only seconds on which two sites agree. See [below](#us-western-and-ercot-grid-frequency-from-seismometer-hum). | Until about 2010 from 100 samples/s analogue stations (within about a second of the GPS-timed ones). Nothing before 2001-09. |
| US Western | `aeso` | 2019-01 → 2025-07 | Alberta system frequency, 2 s. | UTC column runs 3 s late; corrected. |
| US Western | `osf_edr` | 2019-05, 2024-01..11 | KIT GPS recorders in Colorado and Salt Lake City. | — |
| US Western | `lbnl_upmu` | 2015-10-01 → 12 | LBNL micro-PMU in Berkeley, derived from the 120 Hz voltage angle. | — |
| US Eastern | `seismic_east` | 2001-09 → today | Reconstructed from the 60 Hz hum in GPS-timed seismometers; 3–5 stations fused. See [below](#us-eastern-grid-frequency-from-seismometer-hum). | Weak months refetched 2026-11-12 after a rescreen: 2012 now 67% (was 1%), 2013 91% (was 41%), 2010–2011 97–98%, other years 92–100% except 2001 (76%). Still under 90%: 2001-09/12, 2002-01..05, 2003-09, 2004-11, 2010-03, 2011-12..2012-06, 2012-10..2013-01, 2013-03, 2015-04/05, 2016-02, 2017-10, 2021-10/11 ([details](docs/grids/seismic.md#rescreen-of-20102013-2026-11-12)). |
| US Eastern | `osf_edr` | 2023-06/07 | KIT recorder in Florida. | Not GPS-locked: drifts −4 s to −13 s against `seismic_east`, so it ranks below it. |
| ERCOT (Texas) | `seismic_ercot` | 2014-02 → today; days of 2009 | Reconstructed from the 60 Hz hum in GPS-timed seismometers in Dallas–Fort Worth and the Permian Basin; from 2015-02 only seconds on which two sites agree. See [below](#us-western-and-ercot-grid-frequency-from-seismometer-hum). | No station for 2012–2013. One site (Azle) carries most of 2014. Excursions beyond ±250 mHz (2021-02-15) are not tracked. |
| ERCOT (Texas) | `ercot_etts` | ~150 days of 2011–12 | ERCOT EMS system frequency, 4 s. | — |
| ERCOT (Texas) | `osf_edr` | 2019-05 | KIT recorders at two Texas sites. | — |
| India | `grid_india_pmu` | 2020-12-20 → 2023-08-20 | Grid-India PMU at Balipara, Assam, 1 s. | Logged in IST; converted to UTC. |
| Southern Africa | `pmu_zenodo` | 2025-04-28 → 2025-12 | PMU in Stellenbosch, 1 s. | — |
| Japan East (50 Hz) | `dtu_tokyo` | 42 days of 2017, 317 of 2020 | DTU recorder in Tokyo, 0.5 s (CC BY-NC-ND). | — |
| IPS/UPS (Russia & neighbours) | `seismic_ips` | 2022-10 → today (gaps) | Reconstructed from the 50 Hz hum in GPS-timed strong-motion stations in Azerbaijan, Kazakhstan and Kyrgyzstan; only seconds on which two sites agree. See [below](#ipsups-grid-frequency-from-seismometer-hum). | Stations hum only part of the time: 98% of 2025-04 is stored, 74% of 2023-01, 6–15% of the months of 2023-03..2024-10 tried. Sites can differ by up to ~2 s. |
| IPS/UPS (Russia & neighbours) | `seismic_estonia` | 2015-05 → 2025-02-08 (gaps) | Reconstructed from the 50 Hz hum in the Estonian national seismic network (250 samples/s, GPS-timed) while the Baltic states were part of IPS/UPS; only seconds on which two sites agree. See [`docs/grids/seismic.md#ipsups-from-estonia-seismic_estonia`](docs/grids/seismic.md#ipsups-from-estonia-seismic_estonia). | Sites hum by spells, mostly at night: 4–25% of the seconds of the months checked. ~1 s ahead of KIT's recorders (r 0.95–0.996, 2.7 mHz RMS against Kaunas/Tallinn 2025). |
| IPS/UPS (Russia & neighbours) | `seismic_ips_shake` | 2021 → today (gaps) | Raspberry Shakes in Kazakhstan, Kyrgyzstan, Uzbekistan, Georgia, Belarus and Estonia (until 2025-02-08), the 50 Hz line on Nyquist decoded as for China, fused with `seismic_ips`'s stations, which decide where they hum. Fills what `seismic_ips` and `seismic_estonia` leave. See [`docs/grids/seismic.md#ipsups-from-raspberry-shakes-seismic_ips_shake`](docs/grids/seismic.md#ipsups-from-raspberry-shakes-seismic_ips_shake). | Shakes good to ~5 mHz (r 0.945 median against the direct stations). In night probes 2023–2024 go from ~24% of seconds to 96–98%; nothing added before 2021. Backfilled offline. |
| IPS/UPS (Russia & neighbours) | `osf_edr` | 2019-03-29 → 2019-04-17, 2019-04-30 → 05-11 | KIT recorders in Tallinn and St. Petersburg, 1 s. | Tallinn's clock ran 19–55 s late, growing by 1–2 s a day: corrected against `seismic_estonia`, its first four days dropped. |
| China Southern (with Hong Kong, Macau) | `seismic_cn_south` | 2019-07 → 2025-12 (gaps; 326 days in all) | Reconstructed from the 50 Hz hum in Raspberry Shake geophones in Hong Kong and Guangdong. The line sits on the Nyquist frequency of their 100 samples/s; a decoder recovers the signed deviation. See [below](#chinas-grid-frequencies-from-raspberry-shake-hum). | 4–7 mHz per station, against 1 mHz for a direct line. Stations hum by spells. |
| China North & Central | `seismic_cn_north` | 2019-08 → 2020-03, 2022-09 → 2024-09 (483 days) | Same method: Beijing, Tianjin, Ordos. | As above; often one station alone. |
| China East | `seismic_cn_east` | 2023-10 → 2024-04 (53 days) | Same method: Xuzhou, Xiamen. | As above. |
| China Southwest | `seismic_cn_southwest` | 2021-03 → 2024-02 (504 days) | Same method: one station in Chongqing. | One station: nothing to confirm it. |
| Brazil (SIN) | `seismic_brazil` | 2004-02 → 2008-08 (gaps) | Reconstructed from the 60 Hz hum in one University of São Paulo seismic recorder. See [`docs/grids/seismic.md#brazil-seismic_brazil`](docs/grids/seismic.md#brazil-seismic_brazil). | One site; clock within ~2 s of UTC, not GPS-timed. |
| IPS/UPS and Baltic island mode | `osf_baltic` | 2025-01-23 → 2025-02-09 | KIT recorders in Kaunas and Tallinn, 0.1 s; covers the whole Baltic island period of 2025-02-08/09. | 1.3 s early; corrected. |
| Balearic, Iceland, Gran Canaria, Faroe, South Korea, Malaysia, others | `osf_edr`, `kit_zenodo` | days to months each | KIT measurement campaigns. See [`docs/grids/datasets.md`](docs/grids/datasets.md). | — |

Time offsets were measured against GPS-timed KIT recorders and NESO. Storage is one
compressed file per grid, source and month, about 10–14 MB per grid-year
([`docs/grids/compression.md`](docs/grids/compression.md)).

### US Eastern grid frequency from seismometer hum

No operator publishes per-second frequency for the US Eastern Interconnection.
`seismic_east` reconstructs it from the 60 Hz mains hum that GPS-timed seismometers and
accelerometers in buildings pick up, archived openly at EarthScope: 3–5 screened stations
per month are tracked second by second and fused by agreement. Method, validation and
coverage: [`docs/grids/seismic.md#us-eastern-seismic_east`](docs/grids/seismic.md#us-eastern-seismic_east).

### US Western and ERCOT grid frequency from seismometer hum

The same method covers the other two US interconnections where nobody publishes
frequency. `seismic_west` gives the Western Interconnection for 2001-09..2018-12 and from
2025-08 (Alberta's measured series covers the years between): until about 2010 from
short-period stations in Washington, Montana, Idaho and New Mexico that fold the line to
40 Hz, later from borehole and strong-motion sites that agree at r = 0.999–1.000 over
1,700 km. `seismic_ercot` gives Texas from 2014-02, from stations in Dallas–Fort Worth and
the Permian Basin that agree at r = 0.997 over 630 km and are uncorrelated with both
neighbours. Some strong stations at times carry a line that is not the grid's, so a
second is stored only where two sites agree (in Texas from 2015-02; before that one site
stands alone). Neither has been compared with a measured recorder yet. Screening,
checks and limits: [`docs/grids/seismic.md#us-western-seismic_west`](docs/grids/seismic.md#us-western-seismic_west),
[`docs/grids/seismic.md#ercot-seismic_ercot`](docs/grids/seismic.md#ercot-seismic_ercot).

### IPS/UPS grid frequency from seismometer hum

The same method at 50 Hz gives the IPS/UPS area (Russia, Belarus, Kazakhstan, Central
Asia, the South Caucasus) from 2022-10: `seismic_ips` fuses 200 samples/s strong-motion
stations 2,000 km apart in Azerbaijan, Kazakhstan and Kyrgyzstan. For 2015-05 to
2025-02-08, while the Baltic states were still part of the system, `seismic_estonia` does
the same with the Estonian national network's 250 samples/s stations at GEOFON
([`docs/grids/seismic.md#ipsups-from-estonia-seismic_estonia`](docs/grids/seismic.md#ipsups-from-estonia-seismic_estonia)); they also showed
that KIT's 2019 Tallinn recorder ran up to 55 s late, now corrected. Nothing usable was
found before 2015. Ukraine is part of Continental Europe since 2022-03-16 and is
matched there. `seismic_ips_shake` adds 20 Raspberry Shakes (Almaty, Bishkek, Tashkent,
Astana, Tbilisi, Belarus, Estonia) decoded on Nyquist as for China and fused with the
strong-motion stations, which fills most of 2022-04..2024-10, the thinnest stretch. Screening, coverage and what was ruled out:
[`docs/grids/seismic.md#ipsups-from-azerbaijan-and-central-asia-seismic_ips`](docs/grids/seismic.md#ipsups-from-azerbaijan-and-central-asia-seismic_ips).

### China's grid frequencies from Raspberry Shake hum

Mainland China is seven synchronous areas tied by HVDC, and no operator publishes
frequency. Four of them (Southern with Hong Kong and Macau, North & Central, East,
Southwest) are reconstructed from citizen-run Raspberry Shake seismographs, whose
100 samples/s geophones carry the 50 Hz line exactly on their Nyquist frequency. There
the line is a real beat without a sign; the digitiser's filter roll-off (the line is
0.73% weaker per mHz above 50 Hz) and the beat's continuity give the sign back. Checked
against Germany's measured frequency on three dates (r = 0.91–0.98 per station, 4–11 mHz
RMS) and between Chinese sites up to 600 km apart (r = 0.86–0.94). Caveats: a stretch of
tens of minutes occasionally comes out mirrored, the same way at every station, so
agreement does not catch it; most stored seconds rest on one station; coverage follows
when hobbyists' Shakes hum and are switched on. Northeast, Northwest and Yunnan have no
usable station. The decoding is too costly for the dashboard: these months are
backfilled offline and pushed to the bucket. Method, validation and coverage:
[`docs/grids/seismic.md#china-seismic_cn_`](docs/grids/seismic.md#china-seismic_cn_).

### Brazil's grid frequency from seismometer hum

Brazil's SIN (60 Hz) has no published frequency data. `seismic_brazil` covers 2004-02 to
2008-08 from the hum in old 100 samples/s short-period channels archived by the
University of São Paulo, where the line appears folded to 40 Hz. It is one recorder: a
second channel confirms the first when its own hum is strong enough, and the clock was
checked to about 2 s against earthquake arrivals. Nothing open was found that carries the
line after 2008-08: modern digitisers filter it out. Details and limits:
[`docs/grids/seismic.md#brazil-seismic_brazil`](docs/grids/seismic.md#brazil-seismic_brazil).

### Western Australia and Darwin from seismometer hum

AEMO's frequency data stops at the NEM's edge: the western half of Australia runs on
grids of its own, and none of their operators publishes frequency. Geoscience
Australia's stations carry no usable line and the school seismometers filter it out.
Two things do carry it. A research array across the Perth Basin recorded at
250 samples/s for the first half of 2017, and eight of its sites show the line
directly: `seismic_perth_basin` stores the seconds on which two of them agree (median
r = 0.98 between sites up to 136 km apart). And the Raspberry Shakes of Perth and
Darwin carry it on their Nyquist frequency, as in China: `seismic_swis` fuses up to six
of sixteen stations between Toodyay and Harvey (170 km), whose pairs agree at a median
r = 0.90 over 53 probe dates and which follow a direct GPS-timed line 380 km away at
r = 0.81–0.91 on the two dates it could be compared. That comparison also showed the
decoder answering 0 while the grid stood at +130 mHz, beyond its range; such seconds
are now left empty. On
10 January 2020 two Perth stations show the South West's load-shedding event as AEMO's
investigation report times it (the trip at 12:08:30 UTC, 49.0 Hz at 12:10:23, load
shedding at 48.75 Hz at 12:12:08; read off the stations' spectra in 30 s windows), which ties the stations to the grid, their
clocks to UTC and the decoder's sign to a known fall. `seismic_darwin` is one address
in Darwin, uncorrelated with Perth and checked against nothing. The Pilbara, Mount Isa and Alice Springs have no
station. Details: [`docs/grids/seismic.md#western-australia-and-darwin-seismic_swis-seismic_perth_basin-seismic_darwin`](docs/grids/seismic.md#western-australia-and-darwin-seismic_swis-seismic_perth_basin-seismic_darwin).

### Limits

- **No hum, no answer.** Battery devices far from wiring, noise suppression, VoIP and
  heavy compression can remove the hum. The tool reports this rather than guessing.
- **Grid, not city.** All points on one synchronous grid share the same ENF.
- **Short clips** (about 2 minutes or less) are information-limited.
- **Edit detection** relies on the hum's phase alone: about half of all cuts are found
  when the hum is clear, fewer when it is faint.
- **Coverage.** A grid without reference data at the recording time can be neither
  confirmed nor excluded. Japan West, Québec, Taiwan, Australia's Pilbara, Mount Isa and Alice Springs systems and China's Northeast, Northwest and Yunnan grids have no open source; Western Australia's South West grid is covered for 2017-01..06 and from 2022, Darwin by spells, the latter two only where a Raspberry Shake hummed; the other Chinese grids are covered only where a Raspberry Shake hummed; IPS/UPS has none before
  2015-05 and only spells (mostly nights) until 2022-10;
  Australia has no data for 2017-04..2024-11; ERCOT has almost none before 2014 and the
  US Western Interconnection none before 2001-09; Brazil is covered only for 2004-02..2008-08.

---

## 2. Aircraft

Documentation: [`docs/aircraft/architecture.md`](docs/aircraft/architecture.md)

### Principle

A propeller aircraft drones at a steady pitch (its blade-pass frequency, ~60–200 Hz, and
harmonics). Flying past a microphone, the pitch falls as it goes by: how far it falls
gives the speed, how fast it falls the closest distance, and the details of the curve
the exact path. ADS-B history records where every equipped aircraft was, second by
second. A place that heard the right aircraft at the right distance and speed, at the
right moments, and heard no other between, is where the recording was made. The tones
are far below what anyone notices, pass through walls and survive phone codecs.

### Method

1. **Flyovers.** A whitened spectrogram of the recording is scanned with Doppler
   templates (every speed, closest distance and rest frequency at once); each detection
   is measured as ranges of CPA time, speed, slant distance and rest frequency, with its
   harmonics.
2. **Flight history.** ADSB.lol's daily archives (~4 GB/day) are streamed into a compact
   store of the day's non-jet tracks (`geoloc aircraft fetch DAY`), with altitudes put
   above the WGS84 ellipsoid; ground heights from the Copernicus 90 m DEM.
3. **Corridors.** For each flyover, the 200 m cells along every flight of the right speed
   at the right slant distance.
4. **One clock.** Cells where the flights' closest approaches are heard as far apart as
   the flyovers in the recording; subsets of the flyovers are tried, so clutter (boats,
   cars) stays unmatched.
5. **Silence.** Cells that would have heard another aircraft pass closer, unheard,
   between the flyovers are dropped.
6. **Exact curves.** At the survivors each flight's curve is modelled from its real
   trajectory (emission time solved for every instant) and scored on the spectrogram's
   ridges, with one start time for all flyovers; the best place per combination of
   flights is refined to 20 m.

On the recording from the video that inspired this ([colsto](https://www.youtube.com/watch?v=UytYrhYLOKI)),
searching Long Island for the whole of 2026-09-08 returns the reference place to within
160 m, the same two aircraft and the start second, in a minute. Over the whole continental
US with only the day known, two flyovers are not enough (the reference place ranks third
of 32,000, a weak verdict); with the hum's second, the same recording locates strongly.

### With the hum

`geoloc locate` runs the aircraft stage when ADS-B history is stored: a located hum gives
the second and the grid, where one flyover suffices; a weak hum gives candidate seconds,
one of which two flyovers can confirm; without hum, the aircraft's place and time
narrow the hum search to ± 6 h of one grid. On the reference recording the hum alone is
strong, and at its second the aircraft find the place over the whole US Eastern grid in
90 s. The dashboard then shows the matched aircraft on a street map (track, position
and heading at closest approach, registry details on click) and each flyover's
spectrogram with the curve modelled from the aircraft's track.

### Limits

- Only aircraft with ADS-B in ADSB.lol's coverage; jets are excluded.
- A time window is needed: a day for two or more flyovers, seconds for one.
- Edited files (cuts) are not searched; wind and refraction are not modelled.

## 3. Church bells

## 4. Emergency sirens

---

## Usage

```bash
geoloc refs refresh --grid great_britain        # download reference data
geoloc locate recording.mp4 --json report.json  # analyse a file
geoloc serve                                    # web dashboard on http://127.0.0.1:8000
```

| Command | Purpose |
|---|---|
| `geoloc locate FILE [--grid G] [--from ISO --to ISO] [--no-split] [--json out]` | Full analysis |
| `geoloc serve` | Web dashboard |
| `geoloc grids [--at DATE]` | Synchronous grids and their countries |
| `geoloc refs refresh [--grid G]` | Fetch every month not stored yet (all grids by default) |
| `geoloc refs fetch SOURCE --grid G --months A:B [--force]` | Fetch specific months |
| `geoloc refs list` | Stored references and sources |
| `geoloc refs push \| pull` | Sync the store with the bucket |
| `geoloc refs import-aemo FILE...` | Import AEMO's hand-downloaded yearly workbooks |
| `geoloc locate FILE [--aircraft auto\|on\|off] [--aircraft-bbox W,S,E,N] [--aircraft-fetch]` | Hum and aircraft together |
| `geoloc aircraft events FILE [--json out]` | Flyovers in a recording (CPA time, speed, slant distance, rest frequency) |
| `geoloc aircraft fetch DAY... [--parts A B] [--bbox W,S,E,N]` | Store a day of ADSB.lol history (non-jets) |
| `geoloc aircraft locate FILE --from ISO --to ISO --bbox W,S,E,N [--min-events N] [--json out]` | Aircraft-only search |

Reference data is stored under `~/.cache/geolocate-anything` (or `$GEOLOC_CACHE`); ADS-B
days under `adsb/` there (~0.7 GB each), DEM tiles under `dem/`.

Environment (all optional): `GEOLOC_BUCKET` with `AWS_ENDPOINT_URL`, `AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY`, `AWS_DEFAULT_REGION` mirrors the store to an S3 bucket (and
`refs refresh` uploads to it); `GEOLOC_ADMIN_TOKEN` gates the dashboard's Refresh and
import; `GEOLOC_EARTHSCOPE_REFRESH_TOKEN` enables the EarthScope seismic refreshes (`seismic_east`,
`seismic_ips`; the anonymous EarthScope quota cannot backfill); `GEOLOC_US_SEISMIC_REFRESH`
(with the token) enables `seismic_west` and `seismic_ercot`, ~1.3 TB of download to
backfill, so they are fetched offline and pushed, never refreshed by the dashboard; `GEOLOC_SHAKE_REFRESH` enables the Raspberry Shake
sources (`seismic_cn_*`, `seismic_swis`, `seismic_darwin`, `seismic_ips_shake`), whose decoding costs ~25 CPU-minutes per station and month: they
are backfilled on a separate machine and pushed (`refs push`), never refreshed by the
dashboard; `GEOLOC_AUSPASS_REFRESH` enables `seismic_perth_basin` (a closed 2017 archive, a few GB a month, backfilled the same way); `GEOLOC_GEOFON_REFRESH` enables `seismic_estonia` (a closed period, ~4 GB a month, backfilled the same way); `GEOLOC_BRAZIL_REFRESH` enables `seismic_brazil` refreshes (a closed
2004–2008 archive at ~1.1 GB a month, backfilled offline and pushed the same way);
`GEOLOC_AIRCRAFT_FETCH` lets `locate` (and so the dashboard) download the ADSB.lol day a
recording needs (~4 GB streamed, ~0.7 GB kept; at most `GEOLOC_AIRCRAFT_MAX_FETCH` days per
analysis, default 1), and `GEOLOC_ADSB_KEEP_DAYS` keeps only that many days, the ones used
last (for a bounded volume); `GEOLOC_THREADS` sets how many of their chunks decode at once (default: the
cores, 4 to 8), and how many threads `locate` uses (default: every core): a search
spreads its blocks over them (~30 MB each), long compressed audio is decoded and
resampled in 30 s chunks on them (the same samples as one pass), and the faint-hum
ridge test scores its off-grid nulls on them.

## Building

Requires a C++23 compiler with `#embed` (GCC 15 or Clang 19+), CMake ≥ 3.25, and Boost (Asio, Beast,
JSON), CLI11, libcurl, zstd, FFTW3, libarchive, OpenSSL, zlib and FFmpeg development
packages.

```bash
cmake -S . -B build -G Ninja && cmake --build build   # -> build/geoloc
build/geoloc_tests                                     # offline test suite
```

On Ubuntu 24.04, whose GCC lacks `#embed`, configure with `-DCMAKE_CXX_COMPILER=clang++-20`
(see `.github/workflows/ci.yml` for the full package list).

Build speed: the common headers (Asio, JSON, std) are precompiled (`src/pch.hpp`, turn off
with `-DGEOLOC_PCH=OFF`); Asio's and CLI11's non-template code compiles once, in
`src/*_impl.cpp`; and ccache is used when installed (`-DGEOLOC_CCACHE=OFF` to skip it), so
a fresh build directory or a branch switch recompiles only what changed.

## License

MIT.
