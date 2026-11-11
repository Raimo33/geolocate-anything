"""Build data/tz_areas.json: every grid area cut along time-zone borders.

geoloc compares a file's metadata clock with the time the hum gives (true UTC). When a
device stored local time, the difference is its UTC offset, and only the parts of the
matched grid whose zone had that offset at that instant remain possible. This file
holds those parts: one feature per (grid area, IANA zone) intersection, with
properties {grid, from, to, tz, countries, subdivisions, km2}. The offset itself is
not stored: geoloc asks the system time-zone database at run time, which knows every
zone's DST rules and history.

Inputs (pinned by URL and SHA-256, kept in the cache like build_grid_areas.py's):

    timezone-boundary-builder 2026f, "timezones-1970" shapefile (no oceans)
        Zones that agree since 1970 are merged, as in the tz database's own
        canonical zones, so every zone here has one offset history.
    Natural Earth 10m admin-1 (v5.1.2): which countries / subdivisions a piece covers
    data/grid_areas.json (build_grid_areas.py's output)

The shapefile is read one zone at a time, so memory stays near Natural Earth's (~1 GB).

    uv run --with shapely --with pyshp --with openpyxl python scripts/build_tz_areas.py
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
import shapefile
import shapely
from shapely import STRtree, make_valid, set_precision, union_all
from shapely.geometry import mapping, shape
from shapely.geometry.base import BaseGeometry

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_grid_areas as bga  # noqa: E402  (pinned downloads, Natural Earth)

DATA = bga.DATA
TZ_RELEASE = "2026f"
TZ_FILE = "timezones-1970.shapefile.zip"
bga.PINS[TZ_FILE] = (
    f"https://github.com/evansiroky/timezone-boundary-builder/releases/download/{TZ_RELEASE}/{TZ_FILE}",
    "f9b5e254c22b8d0edd98094e641e083d0a2c9cba8d410b6012abd15aa07d7179",
    None,
)


def _sinus(c: np.ndarray) -> np.ndarray:
    return np.column_stack([c[:, 0] * np.cos(np.radians(c[:, 1])) * 111.32, c[:, 1] * 111.32])


def km2(g: BaseGeometry) -> float:
    """Area in km² on the sinusoidal (equal-area) projection."""
    return 0.0 if g.is_empty else shapely.transform(g, _sinus).area


def width_km(g: BaseGeometry) -> float:
    """Mean width, 2·area/perimeter: a few km for a sliver between two borders drawn from
    different sources, tens of km for any real area however small."""
    if g.is_empty:
        return 0.0
    p = shapely.transform(g, _sinus)
    return 2 * p.area / p.length if p.length else 0.0


def polygons(g: BaseGeometry) -> BaseGeometry:
    """Only the areal parts of a geometry (intersections also yield lines and points)."""
    parts = [p for p in getattr(g, "geoms", [g]) if p.geom_type in ("Polygon", "MultiPolygon") and not p.is_empty]
    return union_all(parts) if parts else shapely.Polygon()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tolerance", type=float, default=0.01, help="simplification, degrees (as grid_areas.json)")
    ap.add_argument("--min-km2", type=float, default=20.0, help="drop pieces smaller than this")
    ap.add_argument("--sliver-km2", type=float, default=2000.0,
                    help="a piece under this size must also hold --sliver-share of its zone (else a border sliver)")
    ap.add_argument("--sliver-share", type=float, default=0.10)
    ap.add_argument("--min-width-km", type=float, default=4.0, help="drop thinner pieces (border slivers), whatever their size")
    ap.add_argument("--share", type=float, default=0.3, help="a subdivision belongs to a piece holding this share of it")
    ap.add_argument("--out", default=str(DATA / "tz_areas.json"))
    ap.add_argument("--accept-new", action="store_true")
    args = ap.parse_args()

    print(f"boundaries cache: {bga.boundaries_dir()}")
    tz_zip = bga.fetch(TZ_FILE, args.accept_new)
    ne = bga.NaturalEarth(bga.fetch("ne_admin1.geojson", args.accept_new))
    ne_geoms = [g for *_, g in ne.feats]
    ne_tree = STRtree(ne_geoms)
    ne_area: dict[int, float] = {}

    grid_feats = json.loads((DATA / "grid_areas.json").read_text())["features"]
    grid_geoms = [make_valid(shape(f["geometry"])) for f in grid_feats]
    grid_tree = STRtree(grid_geoms)

    out: list[dict] = []
    zones_of_country: dict[str, set[str]] = defaultdict(set)
    with shapefile.Reader(str(tz_zip)) as r:
        n = len(r)
        for k, sr in enumerate(r.iterShapeRecords()):
            tz = sr.record["tzid"]
            z = make_valid(shape(sr.shape.__geo_interface__)).simplify(args.tolerance / 2, preserve_topology=True)
            zone_km2 = km2(z)
            pieces = 0
            for i in grid_tree.query(z, predicate="intersects"):
                piece = polygons(make_valid(grid_geoms[int(i)].intersection(z)))
                if km2(piece) < args.min_km2:
                    continue
                piece = polygons(make_valid(set_precision(piece.simplify(args.tolerance, preserve_topology=True), 0.0005)))
                a = km2(piece)
                if a < args.min_km2 or (a < args.sliver_km2 and a < args.sliver_share * zone_km2) or width_km(piece) < args.min_width_km:
                    continue
                subs, countries = [], set()
                largest = (0.0, -1)  # a piece too small to hold a share of any subdivision is named after the one it overlaps most
                for j in ne_tree.query(piece, predicate="intersects"):
                    j = int(j)
                    if j not in ne_area:
                        ne_area[j] = km2(ne_geoms[j])
                    ov = km2(polygons(ne_geoms[j].intersection(piece)))
                    largest = max(largest, (ov, j))
                    if ne_area[j] > 0 and ov >= args.share * ne_area[j]:
                        countries.add(ne.feats[j][0])
                        subs.append(ne.feats[j][1])
                if not countries and largest[1] >= 0:
                    countries.add(ne.feats[largest[1]][0])
                    subs.append(ne.feats[largest[1]][1])
                for c in countries:
                    zones_of_country[c].add(tz)
                p = grid_feats[int(i)]["properties"]
                out.append({
                    "type": "Feature",
                    "properties": {"grid": p["grid"], "from": p.get("from"), "to": p.get("to"), "tz": tz,
                                   "countries": sorted(countries), "subdivisions": sorted(set(subs)), "km2": round(a)},
                    "geometry": mapping(piece),
                })
                pieces += 1
            if pieces:
                print(f"  [{k + 1}/{n}] {tz}: {pieces} piece(s)")
    # Subdivisions only matter where a country spans several zones; elsewhere the country says it all.
    split = {c for c, zs in zones_of_country.items() if len(zs) > 1}
    for f in out:
        f["properties"]["subdivisions"] = [s for s in f["properties"]["subdivisions"] if s.split("-")[0] in split]
    doc = {"type": "FeatureCollection", "tz_release": TZ_RELEASE, "features": out}
    Path(args.out).write_text(json.dumps(doc, separators=(",", ":")))
    print(f"wrote {args.out} ({Path(args.out).stat().st_size / 1024:.0f} KiB, {len(out)} features)")


if __name__ == "__main__":
    main()
