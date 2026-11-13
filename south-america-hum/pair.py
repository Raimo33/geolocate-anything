import sys, numpy as np
sys.path.insert(0, sys.argv[1]); from hum import fetch, track
from obspy import UTCDateTime
base, f0, t0, dur = sys.argv[2], float(sys.argv[3]), UTCDateTime(sys.argv[4]), float(sys.argv[5])
T = {}
for spec in sys.argv[6:]:
    n,s,l,c = spec.split('.'); tr = fetch(base,n,s,l,c,t0,dur)
    if tr is None or tr.stats.npts < 0.5*dur*tr.stats.sampling_rate: print(spec,'no data'); continue
    f, snr = track(tr, f0, win=8.0, hop=1.0, half=0.3); T[spec]=f
    print(f'{spec:16s} start={tr.stats.starttime} snr={snr:.1f} med={np.median(f):.4f} std={np.std(f)*1e3:.1f}mHz  first10(mHz dev)=', np.round((f[:10]-f0)*1e3,1))
ks=list(T)
for i in range(len(ks)):
    for j in range(i+1,len(ks)):
        a,b=T[ks[i]],T[ks[j]]; n=min(len(a),len(b)); a=a[:n]-np.median(a[:n]); b=b[:n]-np.median(b[:n])
        best=max(((np.corrcoef(a[max(0,L):n+min(0,L)], b[max(0,-L):n-max(0,L)])[0,1], L) for L in range(-120,121)), key=lambda x:x[0])
        print(f'  {ks[i]} vs {ks[j]}: r0={np.corrcoef(a,b)[0,1]:.3f}  best r={best[0]:.3f} at lag {best[1]} s')
