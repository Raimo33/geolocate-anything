#!/usr/bin/env python3
"""Score `scripts/eval/run.sh` reports against truth.tsv: one row per (recording, mode).

Hum: the verdict's grid, and its start time minus the true start (YouTube's release_timestamp for a
past livestream). An edited file's verdict names its best segment's start, so that segment's offset
in the file is subtracted. Aircraft: distance from the best fix to the true place.
"""
import csv, glob, json, math, os, sys

H = os.path.dirname(os.path.abspath(__file__))
AUDIO = os.environ.get('EVAL_AUDIO', os.path.expanduser('~/.cache/geolocate-anything/wild/eval-2026-11'))
OUT = os.environ.get('OUT', os.path.join(AUDIO, 'out'))
COLSTO_START = 1788898649.7  # colsto_4CxVxuMVkMc: DOPPLERGEIST's published start (no info.json)
truth = {r['id']: r for r in csv.DictReader(open(os.path.join(H, 'truth.tsv')), delimiter='\t')}


def km(a, b, c, d):
    p = math.pi / 180
    x = math.sin((c - a) * p / 2) ** 2 + math.cos(a * p) * math.cos(c * p) * math.sin((d - b) * p / 2) ** 2
    return 12742 * math.asin(math.sqrt(x))


def seg_offset(rep, cand):
    for sg in rep.get('segments') or []:
        if ((sg.get('verdict') or {}).get('candidate') or {}).get('start_utc') == cand.get('start_utc'):
            return sg['start_s']
    return 0.0


rows = []
for path in glob.glob(os.path.join(OUT, '*.json')):
    mode, _, id_ = os.path.basename(path)[:-5].partition('-')
    t = truth.get(id_)
    if not t:
        continue
    rep = json.load(open(path))
    info = os.path.join(AUDIO, id_ + '.info.json')
    start = json.load(open(info))['release_timestamp'] if os.path.exists(info) else COLSTO_START
    v = rep.get('verdict') or {}
    c = v.get('candidate') or {}
    err = path[:-5] + '.err'
    wall = next((l.split()[-1] for l in open(err) if 'Elapsed' in l), None) if os.path.exists(err) else None
    a = rep.get('aircraft') or {}
    b = a.get('best') or {}
    rows.append(dict(
        id=id_, mode=mode, kind=t['kind'], tier=t['tier'], place=t['place'], minutes=round(rep['duration_s'] / 60),
        hum_hz=(rep.get('hum') or {}).get('nominal_hz'), level=v.get('level'), grid=v.get('grid'),
        grid_ok=(v.get('grid') == t['grid']) if v.get('grid') else None,
        dt_s=round(c['start_utc'] - seg_offset(rep, c) - start) if c.get('start_utc') else None,
        flyovers=len(a.get('events') or []), air_level=b.get('level'),
        air_km=round(km(float(t['lat']), float(t['lon']), b['lat'], b['lon']), 2) if b.get('lat') is not None else None,
        wall=wall))

rows.sort(key=lambda r: (r['kind'], r['tier'], r['place'], r['mode']))
if '--json' in sys.argv:
    json.dump(rows, sys.stdout, indent=1)
    sys.exit()
fmt = '{:4} {:5} {:9} {:30} {:>4} {:>4} | {:8} {:20} {:>6} | {:>3} {:8} {:>9} | {}'
print(fmt.format('kind', 'tier', 'mode', 'place', 'min', 'hum', 'level', 'grid', 'dt s', 'fly', 'air', 'air km', 'wall'))
for r in rows:
    grid = (r['grid'] or '-') + ('' if r['grid_ok'] in (None, True) else ' (WRONG)')
    print(fmt.format(r['kind'], r['tier'], r['mode'], r['place'][:30], r['minutes'], r['hum_hz'] or '-', r['level'] or '-',
                     grid, '-' if r['dt_s'] is None else r['dt_s'], r['flyovers'], r['air_level'] or '-',
                     '-' if r['air_km'] is None else r['air_km'], r['wall'] or 'killed'))
