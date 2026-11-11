# pitchcheck.py manifest... : compares the root written in a manifest with the pitch measured in a few of its samples (reads WAV data only)
import sys, numpy as np, wave
def rd(p):
    w=wave.open(p); n=w.getnframes(); ch=w.getnchannels(); sw=w.getsampwidth(); sr=w.getframerate(); b=w.readframes(min(n,sr*3))
    if sw==3:
        a=np.frombuffer(b,dtype=np.uint8).reshape(-1,3); x=(a[:,0].astype(np.int32)|(a[:,1].astype(np.int32)<<8)|(a[:,2].astype(np.int32)<<16)); x=np.where(x>=1<<23,x-(1<<24),x)/8388608.0
    elif sw==2: x=np.frombuffer(b,dtype=np.int16)/32768.0
    else: x=np.frombuffer(b,dtype=np.float32)
    return x.reshape(-1,ch)[:,0], sr
for m in sys.argv[1:]:
    rows=[l.split(' ',3) for l in open(m)]
    for r in [rows[len(rows)//4], rows[len(rows)//2], rows[3*len(rows)//4]]:
        x,sr=rd(r[3].strip()); seg=x[int(0.1*sr):int(0.1*sr)+32768]
        if len(seg)<32768: seg=np.pad(seg,(0,32768-len(seg)))
        X=np.abs(np.fft.rfft(seg*np.hanning(32768))); f=np.fft.rfftfreq(32768,1/sr); hps=X.copy()
        for k in (2,3): hps[:len(X)//k]*=X[::k][:len(X)//k]
        f0=f[np.argmax(hps[20:len(X)//3])+20]; print(m.split('/')[-1], 'root', r[0], 'measured', round(69+12*np.log2(f0/440),1))
