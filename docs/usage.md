# Usage, configuration and building

## Commands

| Command | Purpose |
|---|---|
| `geoloc locate FILE [--grid G] [--from ISO --to ISO] [--no-split] [--json out]` | Full analysis |
| `geoloc serve` | Web dashboard |
| `geoloc grids [--at DATE]` | Synchronous grids and their countries |
| `geoloc refs refresh [--grid G]` | Fetch every month not stored yet (all grids by default) |
| `geoloc refs fetch SOURCE --grid G --months A:B [--force]` | Fetch specific months |
| `geoloc refs list` | Stored references and sources |
| `geoloc refs push \| pull [--lazy]` | Sync the store with the bucket (`--lazy`: manifests and each source's newest month; the rest come on first read) |
| `geoloc refs import-aemo FILE...` | Import AEMO's hand-downloaded yearly workbooks |
| `geoloc locate FILE [--aircraft auto\|on\|off] [--aircraft-bbox W,S,E,N] [--aircraft-fetch]` | Hum and aircraft together |
| `geoloc aircraft events FILE [--json out]` | Flyovers in a recording (CPA time, speed, slant distance, rest frequency) |
| `geoloc aircraft fetch DAY\|FIRST..LAST... [--push [--drop]] [--force] [--parts A B] [--bbox W,S,E,N]` | Store days of ADSB.lol history (non-jets), newest first; `--push` uploads each to the aircraft bucket (skipping days it holds), `--drop` then frees the local copy |
| `geoloc aircraft push [DAY...]` \| `pull [DAY...]` | Sync ADS-B days with the aircraft bucket (`pull` alone: every day's coverage sidecar) |
| `geoloc aircraft index [DAY...] [--force]` | Build stored days' chunk index (`<day>.idx`, ~3 MB, from the store alone; otherwise built on first use) |
| `geoloc aircraft locate FILE --from ISO --to ISO --bbox W,S,E,N [--min-events N] [--no-type-prior] [--json out]` | Aircraft-only search (`--no-type-prior`: keep aircraft whose type cannot make a flyover's tone) |
| `geoloc aircraft learn-f0 REPORT.json... [--min-level moderate] [--list]` | Add confirmed matches' rest frequencies to the per-type blade-pass table (`<cache>/adsb/blade_pass_learned.json`) |

## Storage and environment

Reference data is stored under `~/.cache/geolocate-anything` (or `$GEOLOC_CACHE`); ADS-B
days under `adsb/` there (~50 MB each, plus a ~3 MB chunk index `<day>.idx` built from it), DEM tiles under `dem/`.

Environment (all optional): `GEOLOC_GRID_BUCKET` with `GEOLOC_S3_ENDPOINT_URL`, `GEOLOC_GRID_ACCESS_KEY_ID`,
`GEOLOC_GRID_SECRET_ACCESS_KEY`, `GEOLOC_S3_REGION` mirrors the store to an S3 bucket (and
`refs refresh` uploads to it); with it, a month a manifest lists but this machine lacks is
downloaded when first read (a search first fetches every month of its candidate grids,
16 at a time), so the dashboard keeps no copy of its own: on start it pulls only the
manifests and each source's newest month (`refs pull --lazy`); `GEOLOC_REFS_KEEP_MB` bounds the local copy, evicting the
months fetched from the bucket that were not read for 10 minutes (months written here stay
until uploaded); `GEOLOC_ADMIN_TOKEN` gates the dashboard's Refresh and
import; `GEOLOC_EARTHSCOPE_REFRESH_TOKEN` enables the EarthScope seismic refreshes (`seismic_east`,
`seismic_ips`; the anonymous EarthScope quota cannot backfill); `GEOLOC_US_SEISMIC_REFRESH`
(with the token) enables `seismic_west` and `seismic_ercot`, ~1.3 TB of download to
backfill, so they are fetched offline and pushed, never refreshed by the dashboard; `GEOLOC_SHAKE_REFRESH` enables the Raspberry Shake
sources (`seismic_cn_*`, `seismic_swis`, `seismic_darwin`, `seismic_ips_shake`), whose decoding costs ~25 CPU-minutes per station and month: they
are backfilled on a separate machine and pushed (`refs push`), never refreshed by the
dashboard; `GEOLOC_AUSPASS_REFRESH` enables `seismic_perth_basin` (a closed 2017 archive, a few GB a month, backfilled the same way); `GEOLOC_GEOFON_REFRESH` enables `seismic_estonia` (a closed period, ~4 GB a month, backfilled the same way); `GEOLOC_BRAZIL_REFRESH` enables `seismic_brazil` refreshes (a closed
2004–2008 archive at ~1.1 GB a month, backfilled offline and pushed the same way);
`GEOLOC_AIRPLANE_BUCKET` with `GEOLOC_AIRPLANE_ACCESS_KEY_ID`, `GEOLOC_AIRPLANE_SECRET_ACCESS_KEY`
(endpoint and region from `GEOLOC_S3_*`) is the ADS-B store's own bucket: days are built
offline (`aircraft fetch FIRST..LAST --push --drop`) and kept there; a server pulls every
day's coverage sidecar on start (the history map) and streams a day's store into its
volume when an analysis first needs it; the dashboard's aircraft tab **Fetch the latest data** (admin)
fetches whichever of the last 7 UTC days are missing from ADSB.lol and uploads them; `GEOLOC_AIRPLANE_FETCH` lets `locate` (and so the
dashboard) download an ADSB.lol day the bucket lacks (~4 GB streamed, ~50 MB kept, then
uploaded to the bucket; at most `GEOLOC_AIRPLANE_MAX_FETCH` days per analysis, default 1), and
`GEOLOC_AIRPLANE_KEEP_DAYS` keeps only that many days' stores, the ones used last (for a
bounded volume); `GEOLOC_THREADS` sets how many of their chunks decode at once (default: the
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

Build speed: Clang compiles this code ~30% faster than GCC (`-DCMAKE_CXX_COMPILER=clang++`;
the Docker image and CI use it); the common headers (Asio, JSON, std) are precompiled
(`src/pch.hpp`, turn off with `-DGEOLOC_PCH=OFF`); Asio's and CLI11's non-template code
compiles once, in `src/*_impl.cpp`; the slowest sources are listed first so Ninja never ends
on one long job; and ccache is used when installed (`-DGEOLOC_CCACHE=OFF` to skip it), so
a fresh build directory or a branch switch recompiles only what changed.
