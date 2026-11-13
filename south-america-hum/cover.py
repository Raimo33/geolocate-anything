import sys, io, urllib.request, urllib.parse, numpy as np
sys.path.insert(0, sys.argv[1]); from hum import track
from obspy import read, UTCDateTime
base, f0 = sys.argv[2], float(sys.argv[3]); stas = sys.argv[4].split(','); dates = sys.argv[5:]
def get(spec, t0, dur):
    n,s,l,c = spec.split('.')
    q = urllib.parse.urlencode(dict(net=n, sta=s, loc=l or '--', cha=c, start=str(t0)[:19], end=str(t0+dur)[:19]))
    try:
        b = urllib.request.urlopen(base+'?'+q, timeout=120).read()
        return read(io.BytesIO(b)) if b else None
    except Exception: return None
def abs_track(st, t0, dur, f0):
    st.merge(method=1, fill_value=0); tr = st[0]; tr.trim(t0, t0+dur, pad=True, fill_value=0)
    return track(tr, f0, win=8.0, hop=2.0, half=0.3)
for d in dates:
    t0 = UTCDateTime(d); row=[]; T={}
    for sp in stas:
        st = get(sp, t0, 3600)
        if st is None: row.append(f'{sp.split(".")[1]}: -'); continue
        secs = sum(tr.stats.npts/tr.stats.sampling_rate for tr in st); nseg=len(st)
        try: f, snr = abs_track(st, t0, 3600, f0); T[sp]=f; row.append(f'{sp.split(".")[1]}: {secs:.0f}s/{nseg}seg snr={snr:.0f}')
        except Exception as e: row.append(f'{sp.split(".")[1]}: {secs:.0f}s err')
    ks=list(T); rr=''
    for i in range(len(ks)):
        for j in range(i+1,len(ks)):
            a,b=T[ks[i]],T[ks[j]]; n=min(len(a),len(b))
            if n>30: rr+=f' r[{ks[i].split(".")[1]}-{ks[j].split(".")[1]}]={np.corrcoef(a[:n],b[:n])[0,1]:.2f}'
    print(d[:13], ' | '.join(row), '|', rr, flush=True)
