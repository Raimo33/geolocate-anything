#!/usr/bin/env python3
"""Truth table for Praveen Pathak's Livermore passages (Zenodo 10.5281/zenodo.23424278, CC BY 4.0).

The dataset gives, per passage, the audio, a hand-traced frequency curve with UTC times, the ADS-B
track (KML, EGM96 geometric altitude) and the closest slant range R and horizontal offset L. It does
not publish the recorder's coordinates, so they are fitted here: the one point (lat, lon, height
above EGM96) whose closest 3-D distance to every track best matches the published R (robust: cases
off by more than 150 m are dropped and the fit repeated).

  truth.py DATASET_DIR > truth.tsv    # DATASET_DIR holds final_results_40_cases.csv and raw/
Writes the site on stderr and one row per passage: case, wav, start_utc (first traced point), the
ADS-B closest approach (UTC, slant m, speed m/s) seen from the fitted site, and the published values.
"""
import csv, glob, math, os, re, sys
from datetime import datetime, timezone

import numpy as np

D = sys.argv[1]
rows = {r['case']: r for r in csv.DictReader(open(os.path.join(D, 'zip', 'final_results_40_cases.csv')))}
LAT0, LON0 = 37.69, -121.77
KY = 111_132.0
KX = KY * math.cos(math.radians(LAT0))


def iso(s):
    return datetime.fromisoformat(s.replace('Z', '+00:00')).timestamp()


def track(path):
    txt = open(path).read()
    t = np.array([iso(w) for w in re.findall(r'<when\s*>([^<]+)', txt)])
    c = np.array([[float(x) for x in s.split()] for s in re.findall(r'<gx:coord\s*>([^<]+)', txt)])
    x, y = (c[:, 0] - LON0) * KX, (c[:, 1] - LAT0) * KY
    _, keep = np.unique(t, return_index=True)
    return t[keep], x[keep], y[keep], c[keep, 2]


cases = {}
for d in sorted(glob.glob(os.path.join(D, 'raw', '*', '*', '*_R*'))):
    case = os.path.basename(d)
    if case not in rows:
        continue
    kml = glob.glob(os.path.join(d, '*.kml'))[0]
    t, x, y, z = track(kml)
    t0 = iso(rows[case]['received_start_utc'])
    m = (t > t0 - 900) & (t < t0 + 900)  # the pass heard, not the rest of the day
    # Dense resampling (0.5 s) so the closest approach is not limited to the report rate.
    tt = np.arange(t[m].min(), t[m].max(), 0.5)
    cases[case] = dict(dir=d, t=tt, x=np.interp(tt, t[m], x[m]), y=np.interp(tt, t[m], y[m]), z=np.interp(tt, t[m], z[m]),
                       R=float(rows[case]['R_ADSB_m']), t0=t0)


def closest(c, sx, sy, sz):
    r = np.sqrt((c['x'] - sx) ** 2 + (c['y'] - sy) ** 2 + (c['z'] - sz) ** 2)
    i = int(np.argmin(r))
    return r[i], i


def cost(names, sx, sy, sz):
    return np.array([closest(cases[n], sx, sy, sz)[0] - cases[n]['R'] for n in names])


def fit(names, sx, sy, sz):
    for step in (400.0, 100.0, 25.0, 5.0, 1.0):
        best = (np.sum(cost(names, sx, sy, sz) ** 2), sx, sy, sz)
        for dx in (-step, 0, step):
            for dy in (-step, 0, step):
                for dz in (-step / 4, 0, step / 4):
                    e = np.sum(cost(names, sx + dx, sy + dy, sz + dz) ** 2)
                    if e < best[0]:
                        best = (e, sx + dx, sy + dy, sz + dz)
        while True:  # descend at this step until no neighbour is better
            e0, bx, by, bz = best
            for dx in (-step, 0, step):
                for dy in (-step, 0, step):
                    for dz in (-step / 4, 0, step / 4):
                        e = np.sum(cost(names, bx + dx, by + dy, bz + dz) ** 2)
                        if e < best[0]:
                            best = (e, bx + dx, by + dy, bz + dz)
            if best[0] >= e0:
                break
        _, sx, sy, sz = best
    return sx, sy, sz


# Start from the best of a coarse grid over the town.
names = list(cases)
g = min(((np.sum(cost(names, gx, gy, 150.0) ** 2), gx, gy) for gx in np.arange(-8000, 8001, 1000)
         for gy in np.arange(-6000, 6001, 1000)))
sx, sy, sz = fit(names, g[1], g[2], 150.0)
for _ in range(3):
    res = cost(names, sx, sy, sz)
    names = [n for n, r in zip(list(cases), cost(list(cases), sx, sy, sz)) if abs(r) <= 150]
    sx, sy, sz = fit(names, sx, sy, sz)
res = cost(names, sx, sy, sz)
lat, lon = LAT0 + sy / KY, LON0 + sx / KX
print(f'site {lat:.6f} {lon:.6f} h={sz:.1f} m (EGM96); {len(names)}/{len(cases)} cases, RMS {np.sqrt(np.mean(res**2)):.1f} m',
      file=sys.stderr)

print('\t'.join(['case', 'category', 'wav', 'start_utc', 'cpa_utc', 'cpa_s_in_trace', 'slant_m', 'speed_ms', 'R_pub_m',
                 'f0_pub_hz', 'in_fit', 'site_lat', 'site_lon', 'site_h']))
for n, c in cases.items():
    r, i = closest(c, sx, sy, sz)
    v = math.hypot(np.gradient(c['x'])[i], np.gradient(c['y'])[i]) / 0.5
    tc = c['t'][i] + r / 340.0  # reception of the closest-approach emission
    print('\t'.join(map(str, [n, rows[n]['category'], os.path.join(c['dir'], 'audio.wav'), rows[n]['received_start_utc'],
                              datetime.fromtimestamp(tc, timezone.utc).isoformat()[:23] + 'Z', round(tc - c['t0'], 2),
                              round(r), round(v, 1), round(c['R']), rows[n]['MII_f0_Hz'][:7], int(n in names),
                              f'{lat:.6f}', f'{lon:.6f}', f'{sz:.1f}'])))
