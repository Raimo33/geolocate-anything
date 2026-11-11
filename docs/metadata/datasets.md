# Metadata datasets

Data used to read location clues out of a file's metadata.

**Status legend:** ✅ wired into the tool

Before any signal processing, `geoloc` reads the container metadata. Phone videos
often carry a **GPS position** (`com.apple.quicktime.location.ISO6709`, `location`)
and a **creation time**. People forget to strip them surprisingly often.

Even without GPS, the creation time is a location clue once the hum has given the true
UTC: a device clock that differs from it by a real UTC offset shows the device's time
zone (see [architecture.md](architecture.md)).

| Status | Dataset | Covers | Why it's useful |
|---|---|---|---|
| ✅ | **timezone-boundary-builder 2026f, "timezones-1970"** (Evan Siroky, ODbL, from OpenStreetMap) | World, one polygon per IANA zone, zones that agree since 1970 merged | Cut against the grid outlines by `scripts/build_tz_areas.py` into `data/tz_areas.json` (176 pieces, 2.2 MB). Offsets and DST come from the system tz database at run time, so the file holds no rules that could go stale |

## Licences

| Source | Licence / terms |
|---|---|
| timezone-boundary-builder (from OpenStreetMap) | ODbL |
