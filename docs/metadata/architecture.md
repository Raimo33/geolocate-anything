# Time zone from the metadata clock (`src/tzclue.cpp`)

The hum ([grid architecture](../grids/architecture.md)) gives the recording's true UTC start. Devices also write a creation time, and a
large share write **local** time. The difference between the two is then the device's
UTC offset, which cuts a grid along time-zone lines: Continental Europe splits into
UTC+0/+1/+2/+3, Nordic into Finland and the rest, the Australian NEM by state (only
Queensland stays on +10 in summer), US Western into Pacific, Mountain and Arizona. The
time-zone data it uses is described in [datasets.md](datasets.md).

## Reading every stamp

| Field (as FFmpeg exposes it) | Formats | Kind |
|---|---|---|
| `com.apple.quicktime.creationdate` | MOV/MP4 from iPhones and iPads | **explicit** (always carries `+HHMM`) |
| `xmp:createdate`, `exif:datetimeoriginal`, `photoshop:datecreated`, `xmpdm:shotdate` | XMP anywhere in the file (MP4/MOV `XMP_`/`uuid`, WAV `_PMX`, ID3 `PRIV` ...) | explicit if an offset is written |
| `sony:creationdate` | Sony XAVC NonRealTimeMeta XML | explicit |
| `creation_time` with `Z` | MP4/MOV/3GP `mvhd`, Matroska `DateUTC`, ASF, MXF | **utc** by the format's definition (FFmpeg always prints `Z`) |
| `creation_time` without zone | AVI `IDIT` (asctime) | local |
| `date` + time-only `creation_time` | WAV/BWF `bext` origination date and time | **local** (EBU Tech 3285) |
| `date` | QuickTime `©day`, ID3 `TDRC` / `TYER+TDAT+TIME`, Vorbis/FLAC/Opus `DATE`, RIFF `ICRD` | explicit with an offset, utc with `Z`, otherwise local |
| `date_recorded` | Matroska/WebM tags | local |

XMP and Sony's XML sit in boxes FFmpeg does not export, so `file_clues` also searches the
first and last 8 MiB of the file (where cameras put metadata, before or after the media)
for the XML itself. Release, encoding and tagging dates are not used: they date a later
copy, not the recording. A bare `Z` is not a zone statement ("this is UTC"), but `+00:00`
is ("this clock is on UTC+0").

## From stamps to an offset

* **Explicit** stamps state the offset. They are used if the instant they give agrees with
  the hum time to within 300 s (the first or the last sample: some devices stamp when they
  finish writing). A stamp hours away is from a later edit or export, whose zone is the
  editor's, and is left out.
* **Utc and local** stamps: wall clock − hum time is compared with every offset some zone
  actually had *at that instant* (from the tz database, so UTC+3:15 is never accepted).
  Within 300 s of one, that is the device's offset (offsets in use are ≥ 15 min apart).
  0 means the device stored UTC: no clue. A utc-kind field holding local time is the
  common camera bug (GoPro, dashcams) and the most frequent source of the clue.
* Stated offsets outrank inferred ones; stamps that still disagree are all kept.

## From an offset to an area

`scripts/build_tz_areas.py` intersects every dated grid area (`grid_areas.json`) with
timezone-boundary-builder's "1970" zones (zones identical since 1970 are merged, as in
the tz database's canonical zones), one zone at a time, and names the countries and, where
a country spans several zones, the subdivisions in each piece. Border slivers, where the
OSM-based zone borders and Natural Earth's country borders disagree, are dropped by their
mean width (2·area/perimeter < 4 km), or by size (< 2,000 km² and < 10% of their zone).
Real small zones (Broken Hill, the Indiana counties, Andorra) stay.

At run time each piece of the matched grid, active on the matched day, is kept if its
zone's offset at the matched instant (`std::chrono::locate_zone`, the system tz database)
is the device's. Because offsets come from the tz database, DST and historical changes
(Turkey's permanent +3 since 2016, Russia 2011/2014) need nothing in the data file.

Caveats reported with every result: the clock is assumed to show local time where the
device was (a traveller's camera may keep home time; when no part of the grid fits, this
is said), and a clock set by hand may have missed the last DST change. Then the zones
whose *other-season* offset equals the observed one are named (only zones with DST:
Turkey and the Maghreb never are).
