import sys, io, glob, urllib.request, urllib.parse, numpy as np
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, sys.argv[1]); from hum import track
from obspy import read, UTCDateTime
invdir, base, net, cha, f0 = sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], float(sys.argv[6])
date = sys.argv[7] if len(sys.argv) > 7 else None; maxn = int(sys.argv[8]) if len(sys.argv) > 8 else 24; dur = 900
rows = []
for f in glob.glob(invdir+'/z_*.txt'):
    for l in open(f, errors='ignore'):
        p = l.rstrip('\n').split('|')
        if len(p) > 16 and p[0] == net and p[3] == cha:
            try: rows.append((p[1], p[2], UTCDateTime(p[15]), UTCDateTime(p[16]) if p[16] else UTCDateTime(2026,11,1), float(p[4]), float(p[5])))
            except Exception: pass
if not rows: print(net, cha, 'no inventory'); sys.exit()
if date: t0 = UTCDateTime(date)
else:
    a = np.median([r[2].timestamp for r in rows]); b = np.median([r[3].timestamp for r in rows]); t0 = UTCDateTime(int((a+b)/2/86400)*86400 + 15*3600)
sel = {}
for r in rows:
    if r[2] <= t0 and r[3] >= t0+dur: sel[(r[0], r[1])] = r
sel = list(sel.values()); step = max(1, len(sel)//maxn); sel = sel[::step][:maxn]
def one(r):
    q = urllib.parse.urlencode(dict(net=net, sta=r[0], loc=r[1] or '--', cha=cha, start=str(t0)[:19], end=str(t0+dur)[:19]))
    try:
        b = urllib.request.urlopen(base+'?'+q, timeout=120).read()
        st = read(io.BytesIO(b)); st.merge(method=1, fill_value=0); tr = st[0]
        if tr.stats.npts < 0.7*dur*tr.stats.sampling_rate: return None
        tr.trim(t0, t0+dur, pad=True, fill_value=0); f, snr = track(tr, f0, win=8.0, hop=2.0, half=0.3)
        return (r, tr.stats.sampling_rate, f, snr)
    except Exception: return None
with ThreadPoolExecutor(6) as ex: res = [x for x in ex.map(one, sel) if x]
print(f'## {net} {cha} {str(t0)[:16]} f0={f0:g}: {len(sel)} tried, {len(res)} with data; lat/lon ~ {np.mean([r[4] for r in sel]) if sel else 0:.1f},{np.mean([r[5] for r in sel]) if sel else 0:.1f}')
good = [x for x in res if x[3] >= 12]
for r, fs, f, snr in sorted(good, key=lambda x: -x[3])[:8]:
    print(f'   {r[0]:6s} {r[4]:7.2f} {r[5]:7.2f} fs={fs:g} snr={snr:4.1f} med={np.median(f):.4f} std={np.std(f)*1e3:.1f} mHz')
for i in range(len(good)):
    for j in range(i+1, len(good)):
        a, b = good[i][2], good[j][2]; n = min(len(a), len(b)); r = np.corrcoef(a[:n], b[:n])[0,1]
        if r > 0.5: print(f'   ** r({good[i][0][0]},{good[j][0][0]}) = {r:.3f}')
