# South America ENF reference screening (2026-11-09/10)

Quick tracker (8-12 s frames, parabolic peak, 15-60 min windows), not the project's `track_hum`.
Re-verify every "confirmed" item with the project pipeline before bulk download.

## Confirmed
- **Brazil SIN, 2004-02 .. 2008-08** — USP FDSN (`seisrequest.iag.usp.br`), network BL, 100 sps EHZ
  (S-13 short-period, analog telemetry). 60 Hz survives aliased to 40 Hz.
  - BL.CAM1..EHZ and BL.VAO3..EHZ: 38-45 dB, r = 0.95-1.00 in almost every sampled month.
  - BL.CAM4..EHZ: 10-28 dB, r up to 0.98 (third witness, intermittent).
  - BL.VAO, VAO2, CAM3: no line.
  - Archive starts 2002-09; hum too weak/absent before 2004-02 (CAM1 alone 32-34 dB in 2003-11 and 2004-01).
  - About 1 sampled day in 5 has no data. 2004-08 sample had no line.
  - Caveats: CAM1 and VAO3 are 470 km apart yet identical, so the hum is probably picked up at a
    shared recording site (one witness, not two). Segment start times are irregular; absolute
    timing of this old system is unverified. No overlapping independent reference exists.

## Plausible, short
- **Ecuador (synchronous with Colombia), 2022-03-10 .. 04-15** — EarthScope 2N, 485 nodes, DPZ 250 sps.
  Night window 2022-03-20 03:00 UTC: ~8 nodes at 22-32 dB, pairwise r 0.5-0.84 (nodes 12, 336, 493, 144, 410, 3).
  Node 211 has a 45 dB tone at 59.62 Hz (equipment). Daytime window weaker.
- EarthScope XE (same area, 2020-10-21 .. 11-27, DPZ 250): lines at 15-27 dB, not yet cross-checked.

## Ruled out / nothing usable
- Colombia CM (SGC FDSN): HNZ 200 sps (9 sites) and EHZ 100 sps (15 sites), 5 dates 2018-2026: noise only.
- Northern Chile GEOFON IQ / 8F / 5E (2009-2018): noise. 8G Mejillones 2014: MJ25 21 dB, MJ16 16 dB, no agreement.
- Central Chile ZE 2010 (GEOFON): one weak line (GA4S 14 dB). XY 2010: U91S 18 dB alone.
- Chile C1.LR02 (Valdivia): 9-22 dB line 2017-2024 but unstable, no partner. C1 dam stations: no data served.
- Argentina XN (Puna 2022): two weak lines at 50.115 Hz (equipment-like).
- No data served for: XA (1987-88), ZV, ZR, YM, X9, RI, BV, 3B, YL, XS, NB, 1X, 1P, 2G, 3E, 5R, ZN, C SHZ, NQ.
  Not screened: 22 EPZ returned data but no line; 9D (Venezuela 2014, 100 sps) no line.

## Non-seismic
- No open per-second frequency download found for ONS, CAMMESA, Coordinador, XM, COES, ADME.
  ONS CKAN (`dados.ons.org.br`) has only frequency quality indicators (DFP/DFD), not raw frequency.
- UFSC MedFasee Cono Sur page shows live PMU frequency for Chile (7), Argentina (4), Uruguay (1).
  Historian access untested; needs permission from UFSC.
- Untried: VLF/ELF receivers, induction magnetometers, timestamped broadcast archives.
