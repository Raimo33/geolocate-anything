# Blade-pass table: what tone a type can make

`src/air/bladepass.cpp` holds, per ICAO type designator, the tones a propeller
aircraft makes at its rest frequency, so the search can drop flights whose type
cannot have produced a flyover's measured f0 (architecture.md, § 5).

## Model

| Source | Frequency | Notes |
|---|---|---|
| Blade pass | RPM × blades / 60 | Dominant tone of every propeller. Constant-speed props hold the RPM their governor is set to: turboprops within a narrow band (a PC-12 at 1,700 rpm), pistons between ~1,900 (descent, low cruise) and the take-off limit. |
| Piston firing | RPM × cylinders / 2 / 60 | Direct-drive four-strokes only: 4 cylinders give twice the shaft rate (same as a two-blade prop), 6 give three times, a 9-cylinder radial 4.5 times. Geared engines (Rotax, GTSIO-520, Austro/Continental diesels) are given blade-pass only. |

Each entry is a list of (order, RPM range) with order = blades or cylinders / 2. Every
blade count fitted to the type is listed (factory and common STC propellers: e.g. the
Raisbeck/Hartzell five-blade on the King Air 350 and 200, the Hartzell five-blade on the
PC-12 and TBM). The RPM range goes from descent/low-cruise settings to the take-off
limit, so climb, cruise and descent are all inside it.

Matching an event (bladepass.hpp, `PriorOptions`):

- the event's strongest line f0 is tested as k × fundamental for k ∈ {½, 1, 2, 3}
  (the detector can lock onto a harmonic, or onto the subharmonic of a two-engine beat);
  its other lines as fundamentals only;
- every range is widened by 4% (governor overshoot, a type flown off-book, the f0
  estimate; the scoring later sweeps f0 by ± 2%);
- a type absent from the table, an empty designator, and helicopters (their main-rotor
  harmonics, ~13–30 Hz apart, cover the whole band) never constrain.

One aircraft explaining two events (circuits, a return leg): their lines must agree
within 5%, or at a simple ratio (2, 3, ½, ⅓, 3⁄2, 2⁄3).

## Seed values and sources

Propeller RPM limits are from the FAA/EASA type certificate data sheets (TCDS) and the
pilot's operating handbooks' limitations; cruise settings from the same handbooks or
operator documents. Where only the maximum is published, the low end is set ~15–20%
below it, the band constant-speed turboprops are flown in.

| Types | Blades | Prop RPM (low – max) | Fundamental (Hz) | Source / note |
|---|---|---|---|---|
| C150 C152 | 2 | 1,900 – 2,750 | 63 – 92 | O-200 2,750, O-235 2,550 (TCDS 3A19) |
| C172 C72R C177 | 2 (+ 4/6 cyl.) | 1,900 – 2,700 | 63 – 135 | O-320/O-360/IO-360 2,700; early 172 O-300 6 cyl. (TCDS 3A12) |
| C182 C82R C82T | 2, 3 (6 cyl.) | 1,900 – 2,600 | 63 – 130 | O-470 2,600, IO-540 2,400 (TCDS 3A13) |
| C180 C185 C206 T206 C207 C210 T210 P210 C337 | 2, 3 (6 cyl.) | 1,900 – 2,850 | 63 – 143 | IO-520 2,850 (TCDS 3A13, A4CE, 3A21) |
| P28A PA18 PA20 PA22 AA1 AA5 BE23 BE76 BE77 TB10 | 2 (4 cyl.) | 1,900 – 2,700 | 63 – 90 | O-320/O-360 2,700 (TCDS 2A13, 1A10) |
| P28B P28R P28T PA24 PA32 P32R PA46 TB20 TB21 GA8 | 2, 3 (4/6 cyl.) | 1,900 – 2,800 | 63 – 140 | IO-360/IO-540/TIO-540 2,500–2,700 |
| BE33 BE35 BE36 BE24 | 2, 3 (4/6 cyl.) | 1,900 – 2,700 | 63 – 135 | IO-520/IO-550 2,700 (TCDS 3A15) |
| M20P M20J M20K M20R M20T M20M | 2, 3 (4/6 cyl.) | 1,900 – 2,700 | 63 – 135 | IO-360 2,700, IO/TSIO-550 2,500 |
| SR20 SR22 S22T | 2, 3 (6 cyl.) | 1,900 – 2,700 | 63 – 135 | IO-360-ES/IO-550-N 2,700, TSIO-550 2,500 (TCDS A00009CH) |
| DA40 | 3 | 1,900 – 2,400 | 95 – 120 | IO-360 2,400; AE300 prop 2,300 (geared) |
| PA44 BE95 | 2 (4 cyl.) | 1,900 – 2,700 | 63 – 90 | O-360 2,700 |
| PA23 PA30 PA31 PA34 BE55 BE58 C310 C320 C340 C402 C414 BN2P | 2, 3 (4/6 cyl.) | 1,900 – 2,800 | 63 – 140 | IO-520/IO-550/TSIO-360/TIO-540 2,575–2,800 |
| DA42 DA62 | 3 | 1,900 – 2,300 | 95 – 115 | Austro AE300/AE330, prop 2,300 (geared) |
| DHC2 BE18 | 2, 3 (9-cyl. radial) | 1,700 – 2,300 | 57 – 173 | R-985 2,300 take-off |
| PC12 | 4, 5 | 1,500 – 1,750 | 100 – 146 | 1,700; NGX low-speed mode 1,550 ([Aviation Week](https://aviationweek.com/business-aviation/pilatus-pc-12ngx-leaping-far-ahead-predecessors)); Hartzell 5-blade standard from 2016 ([AIN](https://live.ainonline.com/aviation-news/business-aviation/2015-11-16/hartzell-stcs-five-blader-all-pc-12s)) |
| PC6 PC7 | 3 | 1,800 – 2,250 | 90 – 113 | PT6A-27/-25A 2,200 |
| B350 BE30 | 4, 5 | 1,400 – 1,750 | 93 – 146 | PT6A-60A 1,700; Raisbeck/Hartzell 5-blade STC ([General Aviation News](https://generalaviationnews.com/2017/09/25/raisbeck-receives-stc-approval-for-five-blade-swept-propellers-for-king-air-350/)) |
| BE20 | 3, 4, 5 | 1,600 – 2,050 | 80 – 171 | PT6A-42 2,000 (EASA TCDS IM.A.277); Raisbeck 4- and 5-blade STCs |
| BE9L BE9T BE10 BE99 | 3, 4 | 1,700 – 2,250 | 85 – 150 | PT6A-21/-28 2,200 |
| B190 | 4 | 1,400 – 1,750 | 93 – 117 | PT6A-65B 1,700 |
| C208 C425 | 3, 4 | 1,600 – 1,950 | 80 – 130 | PT6A-114A/-140 1,900; Hartzell 4-blade STC |
| TBM7 TBM8 TBM9 | 4, 5 | 1,600 – 2,050 | 107 – 171 | PT6A-64/-66D 2,000, TBM 960 1,925 ([Flying](https://www.flyingmag.com/aircraft-turboprops-five-blade-hartzell-prop-proves-beneficial-tbm-owners/)) |
| P46T | 4, 5 | 1,700 – 2,050 | 113 – 171 | PT6A-42A/-52 2,000 |
| KODI | 4, 5 | 1,700 – 2,250 | 113 – 188 | PT6A-34 2,200 |
| DHC6 | 3, 4 | 1,700 – 2,250 | 85 – 150 | PT6A-27 2,200; Raisbeck 4-blade STC |
| PAY1 PAY2 PAY3 PAY4 | 3, 4 | 1,600 – 2,250 | 80 – 150 | PT6A-28/-41 2,000–2,200 |
| C441 MU2 SW3 SW4 JS31 JS32 D228 AC90 AC95 | 3, 4, 5 | 1,300 – 1,650 | 65 – 138 | Garrett TPE331: 100% = 1,591 rpm |
| E120 | 4 | 1,000 – 1,350 | 67 – 90 | PW118 1,300 |
| SF34 | 4 | 1,100 – 1,420 | 73 – 95 | CT7 1,396 |
| DH8A DH8B DH8C | 4 | 850 – 1,230 | 57 – 82 | Hamilton 14SF 1,212 |
| DH8D | 6 | 800 – 1,050 | 80 – 105 | Dowty R408: 1,020 take-off, 900 climb, 850 cruise ([CAA TCDS](https://caa.co.uk/Documents/Download/3940/77d2f18a-4292-443a-8980-607aade5d45a/3401), [Transport Canada](https://tc.canada.ca/en/aviation/commercial-air-services/operational-evaluation-reports/appendix-1-operational-evaluation-report-bombardier-dhc8-400)) |
| AT43 AT44 AT45 AT46 AT72 AT73 AT75 AT76 | 4, 6 | 900 – 1,230 | 60 – 123 | 100% Np 1,200, climb/cruise 82% ([ATR 72-600 factsheet](https://www.atr-aircraft.com/wp-content/uploads/2020/07/Factsheets_-_ATR_72-600.pdf)); -500/-600 six-blade 568F |

The reference case's two aircraft fit: N22NY (PC-12) at 113.8 Hz = 4 × 1,707 rpm, and
N93CQ (King Air 350) at 130.2 Hz = 5 × 1,562 rpm, a five-blade propeller at a typical
cruise setting (with the OEM four blades it would need 1,953 rpm, above the limit).

These are book values; the margins absorb small departures. A wrong entry can drop a
true answer, so `--no-type-prior` turns the prior off, and observations widen it:

## Learning from confirmed matches

`geoloc aircraft learn-f0 REPORT.json... [--min-level moderate]` reads `aircraft
locate --json` or `locate --json` reports and, for the top candidate of each result at
least `--min-level` confident, records each matched aircraft's fitted rest frequency
under its type in `<cache>/adsb/blade_pass_learned.json` (`{"PC12": [113.8, ...]}`).
Each observation adds a ± 3% range to its type (harmonic tolerance and margin apply
as for book ranges). A type not in the seed table starts to constrain once it has three
observations; until then it stays open. `--list` prints what was learned. The file is
per cache: delete it to forget.
