"""Build the grid outlines (data/grid_areas.json) from the dated areas in grids.json.

Every area of every grid (see grids.json "_schema") becomes one GeoJSON feature,
with properties {grid, from, to, status}. A grid's extent at time t is the union
of its features active at t. Area references resolve against these datasets, each
pinned by URL and SHA-256 and kept in the cache (GEOLOC_CACHE/boundaries), not the repo:

    Natural Earth 10m admin-1 (v5.1.2)    ISO countries/subdivisions, islands by point
    US Census cartographic counties 2023   US county outlines (1:5M)
    EIA-861 2024                           which utilities serve each county, and their BA
    MLIT N03 (2024-01-01)                  Japanese municipalities (Niigata, Shizuoka)
    OpenStreetMap via Overpass (dated)     the Fuji and Hime rivers, splitting those prefectures
    ABS ASGS LGA 2024                      Australian local government areas

The US seam between the Eastern, Western and ERCOT interconnections is drawn by
county: each county goes to the interconnection that serves most of its customers
(EIA-861: every utility's counties, customers and balancing authority). HIFLD's
balancing-authority polygons were tried first and rejected: they overlap across the
seam by ~400,000 km² and miss whole ERCOT regions (Midland, Wichita Falls).

After building, the script checks that every polygon is valid, that no two grids
overlap during a common period, and reports land in the countries the grids name
that no grid covers (holes).

    uv run --with shapely --with pyshp --with openpyxl python scripts/build_grid_areas.py

--only china_east,taiwan rebuilds just those grids (it needs only the datasets they use)
and keeps the other outlines as they are in the file.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import sys
import urllib.parse
import urllib.request
import zipfile
from datetime import date
from functools import lru_cache
from pathlib import Path

from shapely import Point, Polygon, make_valid, set_precision, union_all
from shapely.affinity import scale
from shapely.geometry import LineString, MultiLineString, mapping, shape
from shapely.geometry.base import BaseGeometry
from shapely.ops import nearest_points, split

DATA = Path(__file__).resolve().parent.parent / "data"
UA = "geolocate-anything-build/0.2 (grid outlines)"

# --------------------------------------------------------------------------- pinned downloads


def _abs_lga_url(state_code: str) -> str:
    q = {
        "where": f"state_code_2021='{state_code}'",
        "outFields": "lga_name_2024",
        "returnGeometry": "true",
        "outSR": "4326",
        "maxAllowableOffset": "0.002",
        "geometryPrecision": "4",
        "orderByFields": "lga_code_2024",
        "f": "geojson",
    }
    return "https://geo.abs.gov.au/arcgis/rest/services/ASGS2024/LGA/MapServer/1/query?" + urllib.parse.urlencode(q)


# Fixed snapshot date, so the river query returns the same bytes every time.
OVERPASS_DATE = "2026-11-01T00:00:00Z"
RIVERS = {"fuji_river": ("富士川", (35.0, 138.2, 35.9, 138.8)), "hime_river": ("姫川", (36.6, 137.7, 37.1, 138.0))}


def _overpass_query() -> str:
    parts = "".join(f'way["waterway"="river"]["name"="{n}"]({b[0]},{b[1]},{b[2]},{b[3]});' for n, b in RIVERS.values())
    return f'[out:json][timeout:90][date:"{OVERPASS_DATE}"];({parts});out geom;'


# name -> (url, sha256 or None when not yet pinned, POST body or None)
PINS: dict[str, tuple[str, str | None, bytes | None]] = {
    "ne_admin1.geojson": (
        "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/v5.1.2/geojson/ne_10m_admin_1_states_provinces.geojson",
        "22d0e3ad85eb3e27f17cabf8ba2d50e554fbc27a87796ff891d958185da62fb5",
        None,
    ),
    "cb_2023_us_county_5m.zip": (
        "https://www2.census.gov/geo/tiger/GENZ2023/shp/cb_2023_us_county_5m.zip",
        "13b2bcdd81fee8476220793dd1023c4f1d2887945b5f66eef52afa98c99d2485",
        None,
    ),
    "eia861_2024.zip": (
        "https://www.eia.gov/electricity/data/eia861/zip/f8612024.zip",
        "77ce49c60ac5a6bad50c442fc401aad5404a21da875dc5cbaba353af5ede54de",
        None,
    ),
    "N03-20240101_15_GML.zip": (
        "https://nlftp.mlit.go.jp/ksj/gml/data/N03/N03-2024/N03-20240101_15_GML.zip",
        "94d5685ecbe3f85634c2e7c647b5255da62758a89275aacc221cd754e3a7016a",
        None,
    ),
    "N03-20240101_22_GML.zip": (
        "https://nlftp.mlit.go.jp/ksj/gml/data/N03/N03-2024/N03-20240101_22_GML.zip",
        "9c67de5bc446068627f70dd4ca51f29fd3522942a7d5b535190eae99793b8674",
        None,
    ),
    "osm_rivers.json": (
        "https://overpass-api.de/api/interpreter",
        None,
        urllib.parse.urlencode({"data": _overpass_query()}).encode(),
    ),
    "abs_lga_2024_qld.geojson": (_abs_lga_url("3"), None, None),
    "abs_lga_2024_wa.geojson": (_abs_lga_url("5"), None, None),
    "abs_lga_2024_nt.geojson": (_abs_lga_url("7"), None, None),
}
ABS_FILES = {"Queensland": "abs_lga_2024_qld.geojson", "Western Australia": "abs_lga_2024_wa.geojson", "Northern Territory": "abs_lga_2024_nt.geojson"}
PIN_FILE = Path(__file__).resolve().parent / "grid_area_pins.json"


def boundaries_dir() -> Path:
    root = Path(os.environ.get("GEOLOC_CACHE") or Path.home() / ".cache" / "geolocate-anything")
    p = root / "boundaries"
    p.mkdir(parents=True, exist_ok=True)
    return p


def _pins() -> dict[str, str]:
    """SHA-256 per file: the fixed ones above, plus those recorded for live services."""
    out = {k: v[1] for k, v in PINS.items() if v[1]}
    if PIN_FILE.exists():
        out.update({k: v for k, v in json.loads(PIN_FILE.read_text()).items() if k not in out})
    return out


def fetch(name: str, accept_new: bool) -> Path:
    url, _, body = PINS[name]
    path = boundaries_dir() / name
    if not path.exists():
        print(f"  downloading {name} ...")
        req = urllib.request.Request(url, data=body, headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=600) as r:
            data = r.read()
        path.write_bytes(data)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    pins = _pins()
    want = pins.get(name)
    if want is None or (want != digest and accept_new and name not in {k for k, v in PINS.items() if v[1]}):
        recorded = json.loads(PIN_FILE.read_text()) if PIN_FILE.exists() else {}
        recorded[name] = digest
        PIN_FILE.write_text(json.dumps(recorded, indent=1, sort_keys=True) + "\n")
        print(f"  pinned {name} sha256={digest}")
    elif want != digest:
        sys.exit(f"{name}: sha256 {digest} does not match the pinned {want}.\n"
                 f"Delete {path} to download it again, or pass --accept-new to re-pin a live service's answer.")
    return path


# --------------------------------------------------------------------------- Natural Earth

# ISO 3166-2 codes that Natural Earth spells differently or splits further.
ALIASES = {"ES-IB": ["ES-PM"], "ES-CN": ["ES-GC", "ES-TF"]}
# The UK's constituent countries are a property, not a code, in Natural Earth.
GB_NATIONS = {"GB-ENG": "England", "GB-SCT": "Scotland", "GB-WLS": "Wales", "GB-NIR": "Northern Ireland"}
# Never part of a whole-country fill: territories with their own grids, or uninhabited.
OUTLYING = {"NL-BQ1", "NL-BQ2", "NL-BQ3", "NO-X01~", "AU-X03~", "AU-X04~"}


class NaturalEarth:
    def __init__(self, path: Path):
        self.feats = []
        for f in json.loads(path.read_text())["features"]:
            p = f["properties"]
            codes = {p["iso_3166_2"]}
            if p["iso_a2"] == "GB":
                codes |= {c for c, n in GB_NATIONS.items() if p.get("geonunit") == n}
            self.feats.append((p["iso_a2"], p["iso_3166_2"], codes, make_valid(shape(f["geometry"]))))
        self._cache: dict[str, BaseGeometry] = {}

    def matches(self, iso_a2: str, iso2: str, codes: set[str], code: str) -> bool:
        if "-" not in code:  # a whole country
            return iso_a2 == code and iso2 not in OUTLYING
        return bool(codes & set(ALIASES.get(code, [code])))

    def code(self, code: str) -> BaseGeometry:
        if code not in self._cache:
            parts = [g for a, i, c, g in self.feats if self.matches(a, i, c, code)]
            if not parts:
                raise LookupError(f"Natural Earth has no area {code!r}")
            self._cache[code] = union_all(parts)
        return self._cache[code]

    def island(self, name: str, at: tuple[float, float]) -> BaseGeometry:
        """The piece of land holding the point: the polygon of the containing (or, for
        islands Natural Earth draws slightly off, nearest within ~5 km) country's land."""
        pt = Point(at)
        hits = [(a, g) for a, _, _, g in self.feats if g.distance(pt) < 0.05]
        if not hits:  # too small for 1:10m (e.g. Skúvoy): nothing to draw, nothing to cut
            print(f"  island {name!r}: not in Natural Earth (no land within 0.05 deg of {at}), skipped")
            return Polygon()
        a = min(hits, key=lambda h: h[1].distance(pt))[0]
        key = f"land:{a}"
        if key not in self._cache:
            self._cache[key] = union_all([g for x, _, _, g in self.feats if x == a])
        polys = list(getattr(self._cache[key], "geoms", [self._cache[key]]))
        return min(polys, key=lambda p: p.distance(pt))


# --------------------------------------------------------------------------- external datasets


# Balancing authority (EIA code) -> interconnection. EIA-930's BA reference table gives
# each BA's interconnection; SWPP is the SPP RTO's Eastern BA (its 2026 western
# expansion keeps WECC members in their own BAs). NERC region is the fallback for
# utilities reported without a BA (TRE = ERCOT, WECC = Western).
BA_WESTERN = set(
    "AVA AVRN AZPS BANC BPAT CHPD CISO DEAA DOPD EPE GCPD GRIF GRID GWA HGMA IID IPCO LDWP NEVP NWMT PACE PACW "
    "PGE PNM PSCO PSEI SCL SRP TEPC TIDC TPWR WACM WALC WAUW WWA".split()
)
BA_EASTERN = set(
    "AEC AECI CPLE CPLW DUK EEI FMPP FPC FPL GLHB GRIS GVL HST ISNE JEA LGEE MISO NBSO NSB NYIS OVEC PJM SC SCEG "
    "SEC SEPA SOCO SPA SWPP TAL TEC TVA YAD".split()
)
BA_OTHER = {"ERCO": "ercot", "AMPL": "alaska_railbelt", "CEA": "alaska_railbelt", "HECO": "hawaii"}
NERC_ICX = {"TRE": "ercot", "ERCOT": "ercot", "WECC": "us_western", "AK": None, "ASCC": None, "HI": "hawaii", "HICC": "hawaii"}


def _ba_icx(ba) -> str | None:
    if isinstance(ba, str):
        if ba in BA_WESTERN:
            return "us_western"
        if ba in BA_EASTERN:
            return "us_eastern"
        return BA_OTHER.get(ba)
    return None


def _norm_county(name: str) -> str:
    import re
    import unicodedata

    s = unicodedata.normalize("NFKD", str(name)).encode("ascii", "ignore").decode().lower()
    s = re.sub(r"\bst\.?\s", "saint ", s)
    s = re.sub(r"\bste\.?\s", "sainte ", s)
    s = re.sub(r"\b(county|parish|borough|census area|city and borough|municipality)\b", "", s)
    return re.sub(r"[^a-z]", "", s)


@lru_cache(maxsize=1)
def us_county_shapes(accept_new: bool) -> dict[tuple[str, str], BaseGeometry]:
    """(state, normalised county name) -> outline, from the Census cartographic file.
    Virginia's independent cities are keyed with a "city" suffix, as EIA writes them."""
    import shapefile  # pyshp

    with zipfile.ZipFile(fetch("cb_2023_us_county_5m.zip", accept_new)) as z:
        stem = next(n for n in z.namelist() if n.endswith(".shp"))[:-4]
        r = shapefile.Reader(shp=io.BytesIO(z.read(stem + ".shp")), dbf=io.BytesIO(z.read(stem + ".dbf")), shx=io.BytesIO(z.read(stem + ".shx")))
        out = {}
        for rec, shp in zip(r.records(), r.shapes()):
            key = _norm_county(rec["NAME"])
            if rec["NAMELSAD"].lower().endswith(" city") and not key.endswith("city"):
                key += "city"  # independent cities (Virginia, Baltimore, St. Louis), as EIA names them
            out[(rec["STUSPS"], key)] = make_valid(shape(shp.__geo_interface__))
    return out


@lru_cache(maxsize=1)
def us_county_interconnections(accept_new: bool) -> dict[tuple[str, str], tuple[str, float]]:
    """(state, county) -> (interconnection, share of the county's customers on it).

    Each utility's customers in a state (EIA-861 sales, delivery-only and short-form
    tables) are spread evenly over the counties it serves there and counted for its
    balancing authority's interconnection."""
    import collections

    import pandas as pd

    zpath = fetch("eia861_2024.zip", accept_new)
    with zipfile.ZipFile(zpath) as z:
        def xl(name, **kw):
            return pd.read_excel(io.BytesIO(z.read(name)), **kw)

        terr = xl("Service_Territory_2024.xlsx", sheet_name="Counties_States")
        nerc = xl("Utility_Data_2024.xlsx", header=1)
        sales = xl("Sales_Ult_Cust_2024.xlsx", sheet_name="States", header=2)
        deliv = xl("Delivery_Companies_2024.xlsx", header=2)
        short = xl("Short_Form_2024.xlsx")
    region = {(int(r["Utility Number"]), r["State"]): str(r["NERC Region"]) for _, r in nerc.iterrows() if pd.notna(r["Utility Number"])}
    weights: dict[tuple[int, str], collections.Counter] = collections.defaultdict(collections.Counter)

    def add(u, st, ba, n):
        if pd.isna(u):
            return
        icx = _ba_icx(ba)
        if icx is None:
            icx = NERC_ICX.get(region.get((int(u), st), ""), "us_eastern") if region.get((int(u), st)) else None
        if icx:
            n = pd.to_numeric(n, errors="coerce")  # EIA writes "." for withheld values
            weights[(int(u), st)][icx] += max(float(n) if pd.notna(n) else 0.0, 1.0)

    for df, cust in ((sales, "Count.4"), (deliv, "Count.4"), (short, "Total Customers")):
        for u, st, ba, n in zip(df["Utility Number"], df["State"], df["BA Code"], df[cust]):
            add(u, st, ba, n)
    ncty = terr.groupby(["Utility Number", "State"]).size()
    acc: dict[tuple[str, str], collections.Counter] = collections.defaultdict(collections.Counter)
    for u, st, county in zip(terr["Utility Number"], terr["State"], terr["County"]):
        w = weights.get((int(u), st))
        if not w:
            continue
        for icx, n in w.items():
            acc[(st, _norm_county(county))][icx] += n / ncty[(u, st)]
    return {k: (c.most_common(1)[0][0], c.most_common(1)[0][1] / sum(c.values())) for k, c in acc.items()}


# Counties EIA-861 does not settle (no utility listed, or no customers reported):
# the interconnection of the state as a whole.
STATE_DEFAULT = {**{s: "us_western" for s in "WA OR CA NV ID UT AZ NM CO WY MT".split()}, "TX": "ercot", "AK": None, "HI": "hawaii"}


@lru_cache(maxsize=None)
def us_counties_of(icx: str, accept_new: bool) -> BaseGeometry:
    shapes = us_county_shapes(accept_new)
    assign = us_county_interconnections(accept_new)
    keep, defaulted, mixed = [], [], []
    for (st, key), g in shapes.items():
        if st in ("PR", "VI", "GU", "AS", "MP"):
            continue
        got = assign.get((st, key))
        if got is None:
            who = STATE_DEFAULT.get(st, "us_eastern")
            defaulted.append(f"{st}-{key}")
        else:
            who, share = got
            if share < 0.6:
                mixed.append(f"{st}-{key} {share:.0%}")
        if who == icx:
            keep.append(g)
    if icx == "us_eastern":  # report once
        print(f"  US counties: {len(assign)} assigned from EIA-861, {len(defaulted)} by state default, {len(mixed)} split under 60/40")
    return union_all(keep)


@lru_cache(maxsize=None)
def abs_lgas(state: str, accept_new: bool) -> dict[str, BaseGeometry]:
    feats = json.loads(fetch(ABS_FILES[state], accept_new).read_text())["features"]
    out: dict[str, list] = {}
    for f in feats:
        if f["geometry"]:
            out.setdefault(f["properties"]["lga_name_2024"].strip(), []).append(make_valid(shape(f["geometry"])))
    return {k: union_all(v) for k, v in out.items()}


@lru_cache(maxsize=1)
def rivers(accept_new: bool) -> dict[str, BaseGeometry]:
    els = json.loads(fetch("osm_rivers.json", accept_new).read_text())["elements"]
    out = {}
    for key, (name, _) in RIVERS.items():
        lines = [LineString([(p["lon"], p["lat"]) for p in e["geometry"]]) for e in els if e.get("tags", {}).get("name") == name]
        if not lines:
            raise LookupError(f"no OSM ways for {name}")
        out[key] = MultiLineString(lines)
    return out


@lru_cache(maxsize=None)
def n03_side(pref: str, river: str, side: str, accept_new: bool) -> BaseGeometry:
    """A prefecture's municipalities on one side of a river; those the river crosses are
    cut along it. A piece is east when it lies east of the nearest point of the river."""
    zpath = fetch(f"N03-20240101_{pref}_GML.zip", accept_new)
    with zipfile.ZipFile(zpath) as z:
        name = next(n for n in z.namelist() if n.endswith(".geojson"))
        feats = json.loads(z.read(name))["features"]
    line = rivers(accept_new)[river]
    keep = []
    for f in feats:
        g = make_valid(shape(f["geometry"]))
        pieces = []
        for poly in getattr(g, "geoms", [g]):
            if poly.geom_type != "Polygon":
                continue
            pieces.extend(split(poly, line).geoms if poly.intersects(line) else [poly])
        for p in pieces:
            rp = p.representative_point()
            east = rp.x > nearest_points(line, rp)[0].x
            if east == (side == "east"):
                keep.append(p)
    return union_all(keep)


# --------------------------------------------------------------------------- areas


class Resolver:
    def __init__(self, ne: NaturalEarth, accept_new: bool):
        self.ne, self.accept_new = ne, accept_new

    def ref(self, r) -> BaseGeometry:
        if isinstance(r, str):
            return self.ne.code(r)
        if "island" in r:
            return self.ne.island(r["island"], tuple(r["at"]))
        if "polygon" in r:
            return make_valid(Polygon(r["polygon"]))
        if "circle" in r:
            return disc(tuple(r["circle"]), r["km"])
        g = self.dataset(r)
        # Datasets draw borders and coasts their own way; clipping each to the Natural
        # Earth area it lies in keeps neighbours from different datasets from overlapping.
        return make_valid(g.intersection(self.ne.code(r["iso"]))) if r.get("iso") else g

    def dataset(self, r) -> BaseGeometry:
        ds = r["dataset"]
        if ds == "eia861_counties":
            return us_counties_of(r["interconnection"], self.accept_new)
        if ds == "abs_lga_2024":
            lgas = abs_lgas(r["state"], self.accept_new) if "state" in r else {}
            if "names" in r:
                pool = {}
                for st in ABS_FILES:
                    pool.update(abs_lgas(st, self.accept_new))
                missing = [n for n in r["names"] if n not in pool]
                if missing:
                    raise LookupError(f"ABS LGA 2024 has no {missing}")
                return union_all([pool[n] for n in r["names"]])
            unknown = [n for n in r.get("except", []) if n not in lgas]
            if unknown:
                raise LookupError(f"ABS LGA 2024 ({r['state']}) has no {unknown}")
            skip = set(r.get("except", []))
            return union_all([g for n, g in lgas.items() if n not in skip and not n.startswith(("Migratory", "No usual"))])
        if ds == "mlit_n03":
            return n03_side(r["prefecture"], r["of"], r["side"], self.accept_new)
        raise ValueError(f"unknown dataset {ds!r}")

    def area(self, a: dict) -> BaseGeometry:
        g = union_all([self.ref(r) for r in a["include"]])
        if a.get("exclude"):
            g = g.difference(union_all([self.ref(r) for r in a["exclude"]]))
        return make_valid(g)


def disc(at: tuple[float, float], radius_km: float) -> BaseGeometry:
    """A disc of radius_km around a lon/lat point (an ellipse in degrees)."""
    import math

    lon, lat = at
    r = radius_km / 111.32
    return scale(Point(lon, lat).buffer(r, 32), xfact=1 / math.cos(math.radians(lat)), yfact=1, origin=(lon, lat))


def _overlaps(a: dict, b: dict) -> bool:
    lo = max(a.get("from") or "0000", b.get("from") or "0000")
    hi = min(a.get("to") or "9999", b.get("to") or "9999")
    return lo < hi


def km2(g: BaseGeometry) -> float:
    """Rough area in km² of a lon/lat geometry (equal-area at its centroid's latitude)."""
    import math

    if g.is_empty:
        return 0.0
    return g.area * 111.32**2 * math.cos(math.radians(g.centroid.y))


def build(grids: list[dict], res: Resolver) -> list[tuple[dict, dict, BaseGeometry]]:
    out = []
    for g in grids:
        for a in g["areas"]:
            geom = res.area(a)
            if geom.is_empty:
                raise ValueError(f"{g['id']}: an area resolved to nothing")
            out.append((g, a, geom))
        print(f"{g['id']}: {len(g['areas'])} area(s)")
    return out


def check(feats: list[tuple[dict, dict, BaseGeometry]], ne: NaturalEarth, tol_km2: float) -> list[str]:
    errors = []
    for g, a, x in feats:
        if not x.is_valid:
            errors.append(f"{g['id']} {a.get('from')}..{a.get('to')}: invalid geometry")
    for i, (gi, ai, xi) in enumerate(feats):
        for gj, aj, xj in feats[i + 1 :]:
            if gi["id"] == gj["id"] or not _overlaps(ai, aj) or not xi.intersects(xj):
                continue
            ov = km2(xi.intersection(xj))
            if ov > tol_km2:
                errors.append(
                    f"overlap {gi['id']} ({ai.get('from')}..{ai.get('to')}) / {gj['id']} ({aj.get('from')}..{aj.get('to')}): {ov:,.0f} km²"
                )
    return errors


def holes(feats, ne: NaturalEarth, when: str, min_km2: float) -> list[tuple[str, float]]:
    """Land in the countries the grids name that no grid covers at `when`."""
    active = [x for g, a, x in feats if (a.get("from") or "0000") <= when < (a.get("to") or "9999")]
    covered = union_all(active)
    countries = set()
    for g, a, _ in feats:
        for r in a["include"]:
            c = r if isinstance(r, str) else (r.get("iso") or "")
            if c:
                countries.add(c.split("-")[0])
    out = []
    for c in sorted(countries):
        try:
            land = ne.code(c)
        except LookupError:
            continue
        gap = land.difference(covered)
        if km2(gap) > min_km2:
            out.append((c, km2(gap)))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tolerance", type=float, default=0.01, help="simplification, degrees")
    ap.add_argument("--out", default=str(DATA / "grid_areas.json"))
    ap.add_argument("--overlap-km2", type=float, default=75.0, help="largest overlap tolerated (simplification slivers; ~55 km² along China's 10,000 km of inner borders)")
    ap.add_argument("--accept-new", action="store_true", help="re-pin live services (ABS, Overpass) whose answer changed")
    ap.add_argument("--only", default="", help="comma-separated grid ids to rebuild; the others keep their outlines in --out")
    args = ap.parse_args()

    print(f"boundaries cache: {boundaries_dir()}")
    ne = NaturalEarth(fetch("ne_admin1.geojson", args.accept_new))
    grids = json.loads((DATA / "grids.json").read_text())["grids"]
    only = set(filter(None, args.only.split(",")))
    if unknown := only - {g["id"] for g in grids}:
        sys.exit(f"--only: no grid {sorted(unknown)}")
    feats = build([g for g in grids if not only or g["id"] in only], Resolver(ne, args.accept_new))
    simplified = []
    for g, a, x in feats:
        s = make_valid(set_precision(x.simplify(args.tolerance, preserve_topology=True), 0.0005))
        s = union_all([p for p in getattr(s, "geoms", [s]) if p.geom_type in ("Polygon", "MultiPolygon")])
        simplified.append((g, a, s))
    if only:  # the other grids keep the outlines already in the file; the checks still see every grid
        kept = [f for f in json.loads(Path(args.out).read_text())["features"] if f["properties"]["grid"] not in only]
        by_key = {(g["id"], a.get("from"), a.get("to"), a["status"]): (g, a) for g in grids for a in g["areas"]}
        old = []
        for f in kept:
            p = f["properties"]
            key = (p["grid"], p.get("from"), p.get("to"), p["status"])
            if key not in by_key:
                sys.exit(f"--only: {args.out} holds an area of {p['grid']} that grids.json no longer has; build everything")
            old.append((*by_key[key], make_valid(shape(f["geometry"]))))
        order = {g["id"]: i for i, g in enumerate(grids)}
        simplified = sorted(old + simplified, key=lambda t: order[t[0]["id"]])
    errors = check(simplified, ne, args.overlap_km2)
    today = date.today().isoformat()
    for c, k in holes(simplified, ne, today, 2000):
        print(f"  hole ({today}): {c} has {k:,.0f} km² outside every grid")
    out = [
        {
            "type": "Feature",
            "properties": {"grid": g["id"], "from": a.get("from"), "to": a.get("to"), "status": a["status"]},
            "geometry": mapping(x),
        }
        for g, a, x in simplified
    ]
    Path(args.out).write_text(json.dumps({"type": "FeatureCollection", "features": out}, separators=(",", ":")))
    print(f"wrote {args.out} ({Path(args.out).stat().st_size / 1024:.0f} KiB, {len(out)} features)")
    if errors:
        print("\n".join(["CHECKS FAILED:"] + errors))
        sys.exit(1)


if __name__ == "__main__":
    main()
