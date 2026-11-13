import sys, io, json, urllib.request, urllib.parse, itertools
import numpy as np
from obspy import read, UTCDateTime
def fetch(base, net, sta, loc, cha, t0, dur):
    q = urllib.parse.urlencode(dict(net=net, sta=sta, loc=loc or '--', cha=cha, start=str(t0)[:19], end=str(t0+dur)[:19]))
    try:
        r = urllib.request.urlopen(urllib.request.Request(base+'?'+q, headers={'User-Agent':'obspy-like'}), timeout=90)
        b = r.read()
        if not b: return None
        st = read(io.BytesIO(b)); st.merge(method=1, fill_value=0)
        return st[0]
    except Exception as e:
        return None
def track(tr, f0, win=12.0, hop=3.0, half=0.3):
    fs = tr.stats.sampling_rate; x = tr.data.astype(float); x -= x.mean()
    fa = f0 if f0 < fs/2 else abs(fs - f0)          # aliased position
    n = int(win*fs); h = int(hop*fs); nfft = 1<<int(np.ceil(np.log2(n*8)))
    w = np.hanning(n); fr = np.fft.rfftfreq(nfft, 1/fs)
    band = (fr > fa-half) & (fr < fa+half); wide = (fr > fa-3) & (fr < fa+3) & ~((fr > fa-0.7)&(fr < fa+0.7))
    out=[]; snr=[]
    for i in range(0, len(x)-n, h):
        P = np.abs(np.fft.rfft(x[i:i+n]*w, nfft))**2
        k = np.argmax(P[band]); idx = np.where(band)[0][k]
        a,b,c = np.log(P[idx-1:idx+2]+1e-30); d = 0.5*(a-c)/(a-2*b+c)
        f = fr[idx] + d*(fr[1]-fr[0])
        out.append(f if f0 < fs/2 else fs - f); snr.append(10*np.log10(P[idx]/np.median(P[wide])))
    return np.array(out), float(np.median(snr))
if __name__ == '__main__':
    base, f0, t0, dur = sys.argv[1], float(sys.argv[2]), UTCDateTime(sys.argv[3]), float(sys.argv[4])
    res = {}
    for spec in sys.argv[5:]:
        net, sta, loc, cha = spec.split('.')
        tr = fetch(base, net, sta, loc, cha, t0, dur)
        if tr is None or tr.stats.npts < 0.8*dur*tr.stats.sampling_rate: print(f'{spec:18s} no data'); continue
        f, snr = track(tr, f0); res[spec] = f
        print(f'{spec:18s} fs={tr.stats.sampling_rate:g} snr={snr:5.1f} dB  f_med={np.median(f):.4f}  f_std={np.std(f)*1000:6.1f} mHz')
    ks = list(res)
    for a, b in itertools.combinations(ks, 2):
        n = min(len(res[a]), len(res[b]))
        if n > 20:
            r = np.corrcoef(res[a][:n], res[b][:n])[0,1]
            if abs(r) > 0.3: print(f'  r({a},{b}) = {r:.3f}  rms diff = {np.std(res[a][:n]-res[b][:n])*1000:.1f} mHz')
