#!/usr/bin/env python3
"""Score `aircraft events` on the Livermore passages against the ADS-B truth from truth.py.

  score.py TRUTH_TSV EVENTS_DIR [--offset S]

Each recording holds one pass. The detection taken is the event closest in time to the truth's
closest approach (reception time, seconds after the first traced point; the WAV starts a few
seconds earlier, so the median offset is estimated from the clear matches and printed). A pass
counts as found when that event's CPA is within 12 s of the expected time (the phone clock and ADS-B disagree by up to ~8 s, see timing_difference_s) after the offset.
Reported: speed and slant errors of the template value and of the accelerating-pass fit range,
and whether the truth lies inside the event's speed and slant ranges (what the search uses).
"""
import csv, json, os, statistics, sys

truth = list(csv.DictReader(open(sys.argv[1]), delimiter='\t'))
D = sys.argv[2]
off = float(sys.argv[sys.argv.index('--offset') + 1]) if '--offset' in sys.argv else None

rows = []
for t in truth:
    p = os.path.join(D, t['case'] + '.json')
    if not os.path.exists(p):
        continue
    rep = json.load(open(p))
    ev = rep.get('events') or []
    rej = rep.get('rejected') or []
    rows.append((t, ev, rej, rep.get('threshold')))

if off is None:  # the WAV-to-trace offset, from events close to the expected time
    d = []
    for t, ev, _, _ in rows:
        tc = float(t['cpa_s_in_trace'])
        best = min(ev, key=lambda e: abs(e['tc_s'] - tc - 4), default=None)
        if best and abs(best['tc_s'] - tc - 4) < 8 and t['in_fit'] == '1':
            d.append(best['tc_s'] - tc)
    off = statistics.median(d) if d else 0.0
print(f'WAV offset {off:+.2f} s (median over clear matches)')

fmt = '{:12} {:22} {:>5} {:>6} {:>4} | {:>3} {:>3} | {:>7} {:>7} {:>6} | {:>6} {:>7} {:>6} | {:>4} {:>4} | {}'
print(fmt.format('case', 'category', 'v', 'd', 'nev', 'hit', 'rej', 'dtc s', 'dv m/s', 'dd %', 'cov', 'rmse', 'acc', 'v_in', 'd_in', 'note'))
n = hit = vin = din = both = 0
false = 0
for t, ev, rej, thr in rows:
    n += 1
    tc = float(t['cpa_s_in_trace']) + off
    v, d = float(t['speed_ms']), float(t['slant_m'])
    best = min(ev, key=lambda e: abs(e['tc_s'] - tc), default=None)
    ok = best is not None and abs(best["tc_s"] - tc) <= 12
    rbest = min(rej, key=lambda e: abs(e['tc_s'] - tc), default=None)
    rok = rbest is not None and abs(rbest["tc_s"] - tc) <= 12
    false += len(ev) - (1 if ok else 0)
    note = '' if t['in_fit'] == '1' else 'truth unreliable (outside site fit)'
    if v > 180:
        note = (note + '; ' if note else '') + 'faster than the 180 m/s template limit'
    e = best if ok else (rbest if rok else None)
    if e is None:
        print(fmt.format(t['case'], t['category'], round(v), round(d), len(ev), '-', '-', '', '', '', '', '', '', '', '', note))
        continue
    hit += ok
    vi = e['speed_range_ms'][0] - 3 <= v <= e['speed_range_ms'][1] + 3
    di = e['cpa_range_m'][0] / 1.1 <= d <= e['cpa_range_m'][1] * 1.1
    if ok:
        vin += vi
        din += di
        both += vi and di
    rm = e.get('ridge_rmse_hz')
    print(fmt.format(t['case'], t['category'], round(v), round(d), len(ev), 'Y' if ok else '-', 'R' if (rok and not ok) else '',
                     f"{e['tc_s'] - tc:+.1f}", f"{e['speed_ms'] - v:+.0f}", f"{100 * (e['cpa_m'] - d) / d:+.0f}",
                     f"{e.get('ridge_coverage', 0):.2f}", '-' if rm is None else f'{rm:.2f}', f"{e.get('accel_ms2', 0):+.2f}",
                     'Y' if vi else 'n', 'Y' if di else 'n', note))
print(f'\nfound {hit}/{n}; truth inside both ranges {both}/{hit} (speed {vin}, slant {din}); other events {false}')
