# geolocate-anything

An OSINT tool that estimates **where and when an audio or video recording was made** from
background sounds that people rarely notice:

1. [**Grid hum**](#grid-hum-enf): the mains frequency gives the power grid and the exact UTC second.
2. [**Aircraft**](#aircraft): Doppler curves of passing propeller aircraft, matched against
   ADS-B history, give the place to within a few hundred metres.
3. **Church bells** and 4. **Emergency sirens**: planned.

The two implemented signals feed each other: the hum's second lets one flyover locate,
and the aircraft's place and time let a faint hum confirm.

## Grid hum (ENF)

Mains electricity runs at a nominal 50 or 60 Hz, but the actual frequency wanders by a few
millihertz from second to second as supply and demand shift. The wander is identical
everywhere on a synchronous grid and different between grids, and microphones near wiring
pick up the hum. Matching the recording's hum against logged grid frequency identifies
**the grid** and **the UTC second the recording started**.

The tool detects the hum, cuts the file where edits break its phase, extracts the
frequency trace from the harmonics that move together, and cross-correlates it against
every stored second of every candidate grid. The margin over the many wrong alignments
gives a verdict (STRONG / MODERATE / WEAK / NONE). When the file's metadata clock differs
from the hum's UTC by a real offset, it also narrows the grid to the matching time zones.

Reference frequency comes from grid operators and research recorders where they publish it.
Where nobody does (US interconnections, Russia and neighbours, China, Brazil, Western
Australia), it is **reconstructed from the mains hum in open seismometer archives**: stations
are screened, their hum is tracked second by second, and a second is kept where distant
stations agree.

Limits: no hum (battery devices, noise suppression, VoIP) means no answer; the result is a
grid, not a city; clips under about 2 minutes are weak; grids without data for the date
can be neither confirmed nor excluded.

Docs: [design](docs/grids/architecture.md) · [data sources](docs/grids/datasets.md) ·
[seismic reconstruction](docs/grids/seismic.md) · [store format](docs/grids/compression.md) ·
[time zone from metadata](docs/metadata/architecture.md)

## Aircraft

A propeller aircraft drones at a steady pitch, and as it passes a microphone the pitch
falls: how far gives its speed, how fast gives its closest distance. The tool finds these
flyovers in the recording, then searches ADS-B history (ADSB.lol) for the place that heard
the right aircraft, at the right distance and speed, at the right moments, and no other in
between. On the recording that inspired it, the answer lands within 160 m of the true spot.

Limits: only aircraft with ADS-B coverage, jets excluded; it needs a time window (a day for
two or more flyovers, the hum's second for one).

Docs: [design](docs/aircraft/architecture.md) · [store format](docs/aircraft/compression.md) · [blade-pass frequencies](docs/aircraft/blade-pass.md)

## Getting started

### Use the hosted dashboard

A public instance runs at **<https://dashboard-production-00a6.up.railway.app>**: upload a
recording and it runs the full analysis.

### Run it yourself

Run the same dashboard locally with Docker:

```bash
git clone https://github.com/Raimo33/geolocate-anything.git && cd geolocate-anything
docker build -t geoloc .
docker run --rm -p 8000:8000 -v geoloc-data:/data geoloc
```

Open <http://127.0.0.1:8000>, click **Fetch the latest data** to download the grids'
reference data from the public sources (kept in the `geoloc-data` volume), then upload a
recording. Buckets and tokens are optional.

What you already know can narrow the search before you upload: a date range (drag the
slider or type the dates), the mains frequency (50 or 60 Hz), and an area drawn on the map
with a lasso (only the grids inside it are searched, and an aircraft answer must fall inside it).

For the command-line interface, environment variables and building from source, see
[`docs/usage.md`](docs/usage.md).

## License

MIT, see [LICENSE](LICENSE).
