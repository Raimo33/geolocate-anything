# Brazil's grid frequency from seismometer hum (`seismic_brazil`)

Code: the tracker and fusion of [`seismic_east`](seismic_east.md)
(`src/refs/seismic.cpp`, `src/refs/seismic_fetch.cpp`); stations in
`src/refs/seismic_brazil_stations.inc`.

No operator or research group publishes per-second frequency for Brazil's Sistema
Interligado Nacional (SIN, 60 Hz). ONS's open-data portal holds frequency quality
indicators only. `seismic_brazil` reconstructs 2004-02..2008-08 from the mains hum in
old short-period seismic recordings archived by the University of São Paulo.

## Where the hum is

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

## The stray tone, and the median noise floor

Every channel also carries a fixed tone at 40.65 Hz, about 5 dB stronger than the hum.
It is outside the tracker's ±0.25 Hz search, so the frequency track is unaffected, but
it sits among the noise probes (0.6–1.0 Hz from the line). With the usual mean of the
probes the SNR reads 2–3 dB and fusion rejects every second.

`TrackOptions::robust_floor` (set for this network only) takes the median of the 24
probe powers, divided by ln 2 (noise power is exponentially distributed, so its median
is ln 2 of its mean). A tone covering fewer than half the probes then does not count
as noise. `seismic_east` and `seismic_ips` keep the mean and their stored values.

## Verification

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

## Timing

The recorder is not known to be GPS-disciplined. Its clock was checked against the P
arrivals of two earthquakes on `CAM4`, the live channel, using iasp91 travel times:

| Earthquake | Predicted P | Observed onset | Difference |
|---|---|---|---|
| 2005-06-13 Tarapacá M7.8 | 22:49:46.1 | 22:49:44.5 | −1.6 s |
| 2007-11-14 Tocopilla M7.7 | 15:46:12.7 | 15:46:13.7 | +1.0 s |

Both are within the travel-time model's own error at 25°, so the clock is within about
2 s of UTC at those two dates. 2004 and 2008 are unchecked: no data for the 2004 event
tried, and the 2007-08-15 Pisco arrival was too weak to pick.

## How a month is built

Both channels are fetched for the whole month (6 h requests, no account needed).
Where `CAM4` agrees with `CAM1` (r ≥ 0.9 in 10-minute windows) the two are fused;
elsewhere `CAM1` stands alone if its noise estimate is ≤ 1.5 mHz (SNR ≥ 28.5 dB). The
manifest records both counts (`seconds_multi_station`, `seconds_single_station`).

## A fetched month

2005-05, fetched end to end (1.08 GB downloaded: 0.27 GB for `CAM1`, 0.81 GB for `CAM4`):
90.5 % of the month's seconds stored. Of those, 2,244,060 s (92.5 %) rest on both
channels agreeing and 180,764 s (7.5 %) on `CAM1` alone. The missing 9.5 % are gaps in
the archive.

## Limits

- One recorder, one place. A second independent site was not found.
- When `CAM4` is weak, a value rests on `CAM1` alone. It was confirmed by `CAM4` in
  every year from 2004 to 2007, but not in each month.
- Absolute time is good to about 2 s, not to the millisecond as with GPS-timed stations.
- No independent frequency recording overlaps 2004–2008 to compare against.
- The source ranks below measured recorders (rank 60), should any appear.

## What else was screened in South America (2026-11)

Nothing usable: Colombia `CM` (200 samples/s accelerometers and 100 samples/s
short-period, 2018–2026), northern Chile `IQ`, `8F`, `5E`, `8G` (2009–2018), central
Chile `ZE`, `XY` (2010), `C1.LR02`, Argentina `XN` (2022). Ecuador's nodal array `2N`
(EarthScope, 2022-03-10..04-15, 250 samples/s) shows a 60 Hz line agreeing between
nodes at r 0.5–0.84 on a quick tracker; it was not confirmed with `track_hum`.
