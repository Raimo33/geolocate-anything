#!/usr/bin/env python3
"""Score `aircraft locate` on the Livermore passages: distance of the best place to the fitted
recorder site (truth.py), whether the matched flight is the recording's aircraft, and the verdict.

  score_locate.py TRUTH_TSV LOCATE_DIR
"""
import csv, json, math, os, re, sys

truth = list(csv.DictReader(open(sys.argv[1]), delimiter='\t'))
D = sys.argv[2]


def km(a, b, c, d):
    p = math.pi / 180
    x = math.sin((c - a) * p / 2) ** 2 + math.cos(a * p) * math.cos(c * p) * math.sin((d - b) * p / 2) ** 2
    return 12742 * math.asin(math.sqrt(x))


fmt = '{:12} {:22} {:>4} | {:8} {:>8} {:>8} {:>5} | {:10} {:3} | {}'
print(fmt.format('case', 'category', 'fit', 'level', 'best km', 'right km', 'rank', 'flight', 'ok', 'reason'))
n = ok_fl = within = 0
dists = []
for t in truth:
    p = os.path.join(D, t['case'] + '.json')
    if not os.path.exists(p):
        continue
    n += 1
    reg = 'N' + re.match(r'N?([0-9A-Z]+)_R', t['case']).group(1)
    rep = json.load(open(p))
    cands = rep.get('candidates') or []
    lat, lon = float(t['site_lat']), float(t['site_lon'])
    if not cands:
        print(fmt.format(t['case'], t['category'], t['in_fit'], rep.get('level', '-'), '-', '-', '-', '-', '-',
                         '; '.join(rep.get('reasons') or rep.get('notes') or [])[:70]))
        continue
    b = cands[0]
    d = km(lat, lon, b['lat'], b['lon'])
    fl = [m.get('registration') for m in b['matches']]
    good = reg in fl
    rank = next((i + 1 for i, c in enumerate(cands) if reg in [m.get('registration') for m in c['matches']]), None)
    right = next((km(lat, lon, c['lat'], c['lon']) for c in cands if reg in [m.get('registration') for m in c['matches']]), None)
    ok_fl += good
    if t['in_fit'] == '1':
        dists.append(d)
        within += d <= 1.0
    print(fmt.format(t['case'], t['category'], t['in_fit'], rep.get('level', '-'), f'{d:.2f}', '-' if right is None else f'{right:.2f}',
                     rank or '-', ','.join(x or '?' for x in fl)[:10], 'Y' if good else '', '; '.join(rep.get('reasons') or [])[:70]))
dists.sort()
med = dists[len(dists) // 2] if dists else float('nan')
print(f'\n{n} runs; right aircraft first {ok_fl}; best place within 1 km {within}/{len(dists)} (reliable truth); median {med:.2f} km')
