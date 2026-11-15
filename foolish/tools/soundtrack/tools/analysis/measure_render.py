# measure_render.py - measure one engine render (a wav plus its sections.txt) and write a Markdown report, a
#   spectrogram PNG and a per-second level TSV; optionally compare chosen windows with an older render.
#
# What it does: headline loudness (ffmpeg ebur128), the level of the last 12 s and an ending class, the section
#   list verbatim, the tempo map, per-second RMS levels, boundary level tables (largest dB step around chosen
#   section starts, next to a window of the old render), a tail table from one section to the end, and optional
#   band-share and onset-density comparisons between chosen new sections and old time ranges.
# Inputs: --new WAV, --sections FILE (lines: START_SECONDS bar N LABEL), --out DIR; optional --old WAV (the older
#   render to compare with), --name NAME (output stem, default the wav stem), --title, --new-label, --old-label,
#   --boundary TITLE SECTION OLD_T0 OLD_T1 (repeatable; SECTION is a label prefix), --span SECONDS (default 20),
#   --tail TITLE SECTION (table from that section to the end, plus +-3 s steps at every later section start),
#   --band-compare NEW_SECTION NEW_END_SECTION LO_HZ HI_HZ OLD_T0 OLD_T1 (repeatable),
#   --onset-compare NEW_SECTION NEW_END_SECTION OLD_T0 OLD_T1 (repeatable).
# Outputs: DIR/<name>.md, DIR/<name>.png (under 400 kB), DIR/<name>_levels.tsv (t_start_s, rms_dbfs).
# How to run:
#   python3 -I measure_render.py --new out/full.wav --sections out/sections.txt --out reports --old old.wav \
#     --boundary "A -> B" "B" 122 162 --tail "Return to end" "V Return"
# Known failure modes: a section label prefix that matches nothing stops the run with an error; the old render is
#   assumed to be a different arrangement, so its windows are chosen by hand and are only as comparable as you
#   made them; second-level RMS levels hide anything shorter than a second; ffmpeg must be on PATH.
import sys, os, re, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from scipy import signal, ndimage
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from lib import *

FS = 44100

def read_sections(path):
    secs = []
    with open(path) as f:
        for l in f:
            m = re.match(r'\s*([\d.]+)\s+bar\s+(\d+)\s+(.*)', l)
            if m: secs.append((float(m.group(1)), int(m.group(2)), m.group(3).strip()))
    if not secs: raise SystemExit('no section lines (START bar N LABEL) in %s' % path)
    return secs

def section_time(secs, name):
    for s in secs:
        if s[2].startswith(name): return s[0]
    raise SystemExit('no section label starts with %r' % name)

def load(p):
    s = decode44(p); m = s.mean(1); return s, m

def levels(m):
    n = int(len(m)/FS); x = m[:n*FS].reshape(n, FS).astype(np.float64)
    return 10*np.log10((x**2).mean(1)+1e-12)*1.0+0   # dBFS of mean-square = RMS dBFS

def endlev(m):
    return [10*np.log10((m[len(m)-(k+1)*FS:len(m)-k*FS].astype(np.float64)**2).mean()+1e-12) for k in range(11, -1, -1)]

def onset_env(m):
    x = to22(m.astype(np.float32))
    S0 = stft_mag(x, 1024, HOP0); fb = mel_fb(1024, SR, 40, 30, 10000); mm = S0 @ fb.T
    lm = 20*np.log10(mm+1e-6); lm = np.maximum(lm, lm.max()-80)
    fl = np.maximum(0, lm[4:]-lm[:-4]); env = np.concatenate([np.zeros(4), fl.mean(1)])
    fs = SR/HOP0
    es = ndimage.gaussian_filter1d(env, 1.5)
    pk, _ = signal.find_peaks(es, prominence=0.5*es.std(), distance=int(0.05*fs))
    return env, fs, pk/fs

def onsets_per_s(on, t0, t1):
    return [int(((on >= t)&(on < t+1)).sum()) for t in range(int(np.ceil(t0)), int(t1)-0)]

def band_share(m, t0, t1, lo, hi):
    x = m[int(t0*FS):int(t1*FS)].astype(np.float64)
    w = np.hanning(8192); n = (len(x)-8192)//4096
    acc = np.zeros(4097)
    for i in range(n):
        acc += np.abs(np.fft.rfft(x[i*4096:i*4096+8192]*w))**2
    f = np.fft.rfftfreq(8192, 1/FS)
    return acc[(f >= lo)&(f < hi)].sum()/acc.sum()

def tbl(L, t0, t1):
    t0 = max(0, int(round(t0))); t1 = min(len(L), int(round(t1)))
    seg = L[t0:t1]; d = np.diff(seg); k = int(np.argmax(np.abs(d)))
    return t0, seg, (t0+k+1, d[k])

def plot_render(path_png, mn, secs, title, dn):
    x = mn[:].astype(np.float32); n = 4096
    hop = max(512, int(len(x)/1600))
    S = stft_mag(x[::1], n, hop, sr=FS)
    fq = np.fft.rfftfreq(n, 1/FS)
    rows = np.geomspace(30, 16000, 256)
    Sr = np.stack([np.interp(rows, fq, S[i]) for i in range(len(S))])
    db = 20*np.log10(Sr+1e-9); top = db.max()
    fig, ax = plt.subplots(2, 1, figsize=(14, 7), sharex=True, gridspec_kw={'height_ratios': [3, 1]})
    cen = np.arange(len(S))*hop/FS
    ax[0].imshow(db.T, origin='lower', aspect='auto', extent=[0, cen[-1], 0, 256], vmin=top-90, vmax=top, cmap='magma')
    yt = [np.searchsorted(rows, f) for f in (50, 100, 200, 500, 1000, 2000, 5000, 10000)]
    ax[0].set_yticks(yt); ax[0].set_yticklabels(['50', '100', '200', '500', '1k', '2k', '5k', '10k'])
    for s in secs:
        for a in ax: a.axvline(s[0], color='w' if a is ax[0] else 'k', ls='--', lw=0.6, alpha=0.7)
    for i, s in enumerate(secs):
        ax[0].text(s[0]+0.5, 250-(i%4)*14, s[2].split(' (')[0][:22], color='cyan', fontsize=5, rotation=0, va='top')
    ax[0].set_title(title+': log-frequency spectrogram, section boundaries dashed')
    t3, l3 = Loud(mn.astype(np.float64), FS).curve()
    ax[1].plot(t3, l3, lw=0.8); ax[1].set_ylabel('short-term LUFS-like (3 s)'); ax[1].set_xlabel('s'); ax[1].set_xlim(0, dn); ax[1].set_ylim(-45, -8)
    fig.tight_layout()
    for dpi in (90, 75, 60, 50):
        fig.savefig(path_png, dpi=dpi)
        if os.path.getsize(path_png) < 400000: break
    plt.close(fig)
    return os.path.getsize(path_png), dpi

def main():
    ap = argparse.ArgumentParser(description='Measure an engine render (wav + sections.txt); optional comparison with an old render.')
    ap.add_argument('--new', required=True); ap.add_argument('--sections', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--old'); ap.add_argument('--name'); ap.add_argument('--title')
    ap.add_argument('--new-label', default='new'); ap.add_argument('--old-label', default='old')
    ap.add_argument('--boundary', nargs=4, action='append', default=[], metavar=('TITLE', 'SECTION', 'OLD_T0', 'OLD_T1'))
    ap.add_argument('--span', type=int, default=20)
    ap.add_argument('--tail', nargs=2, metavar=('TITLE', 'SECTION'))
    ap.add_argument('--band-compare', nargs=6, action='append', default=[], metavar=('NEW_SECTION', 'NEW_END_SECTION', 'LO_HZ', 'HI_HZ', 'OLD_T0', 'OLD_T1'))
    ap.add_argument('--onset-compare', nargs=4, action='append', default=[], metavar=('NEW_SECTION', 'NEW_END_SECTION', 'OLD_T0', 'OLD_T1'))
    a = ap.parse_args()
    name = a.name or os.path.splitext(os.path.basename(a.new))[0]
    title = a.title or (name+', full render: measurements')
    NL, OL = a.new_label, a.old_label
    old = a.old is not None
    if (a.boundary or a.tail or a.band_compare or a.onset_compare) and not old:
        pass   # new-only tables are produced; comparison blocks need --old
    os.makedirs(a.out, exist_ok=True)
    out = lambda ext: os.path.join(a.out, name+ext)
    secs = read_sections(a.sections)
    T = lambda nm: section_time(secs, nm)
    sn, mn = load(a.new)
    dn = len(mn)/FS
    Ln = levels(mn)
    if old:
        so, mo = load(a.old); do = len(mo)/FS; Lo = levels(mo)
    with open(out('_levels.tsv'), 'w') as f:
        f.write('t_start_s\trms_dbfs\n')
        for i, v in enumerate(Ln): f.write('%d\t%.2f\n' % (i, v))
    I, LRA, TP = ebur128(a.new)
    if old: Io, LRAo, TPo = ebur128(a.old)
    En = endlev(mn)
    envn, fsn, onn = onset_env(mn)
    if old:
        Eo = endlev(mo); envo, fso, ono = onset_env(mo)
    wins, _ = tempo_windows(envn, fsn); wins = viterbi_tempo(wins)
    md = []
    P = md.append
    P('# '+title); P('')
    P('Everything here is DSP measurement of the decoded wav, not a listening verdict.')
    P('Methods follow README.md in the analysis tools folder (same ffmpeg ebur128 summary, same onset envelope and tempo windows code).')
    P('Level per second is the RMS of the mono mix (mean of L and R) over each whole second, in dBFS; second N covers N to N+1 s.'); P('')
    P('File: '+a.new); P('')
    P('## Headline numbers'); P('')
    if old:
        P('| | %s | %s |' % (NL, OL)); P('|---|---|---|')
        P('| duration s | %.2f | %.2f |' % (dn, do)); P('| LUFS integrated | %.1f | %.1f |' % (I, Io)); P('| true peak dBTP | %.1f | %.1f |' % (TP, TPo)); P('| LRA LU | %.1f | %.1f |' % (LRA, LRAo)); P('')
    else:
        P('| | %s |' % NL); P('|---|---|')
        P('| duration s | %.2f |' % dn); P('| LUFS integrated | %.1f |' % I); P('| true peak dBTP | %.1f |' % TP); P('| LRA LU | %.1f |' % LRA); P('')
    P('## Ending'); P('')
    P('RMS level of each of the last 12 seconds (oldest first, last row is the final second), dBFS:'); P('')
    if old:
        P('| seconds before end | %s | %s |' % (NL, OL)); P('|---|---|---|')
        for k in range(12): P('| %d to %d | %.1f | %.1f |' % (12-k, 11-k, En[k], Eo[k]))
    else:
        P('| seconds before end | %s |' % NL); P('|---|---|')
        for k in range(12): P('| %d to %d | %.1f |' % (12-k, 11-k, En[k]))
    last2 = onn[onn >= dn-2]
    P('')
    if old:
        last2o = ono[ono >= do-2]
        P('Onsets detected in the last 2 s: %s %d, %s %d. Drop from 12 s before end to final second: %s %.1f dB, %s %.1f dB.' % (NL, len(last2), OL, len(last2o), NL, En[-1]-En[0], OL, Eo[-1]-Eo[0]))
    else:
        P('Onsets detected in the last 2 s: %d. Drop from 12 s before end to final second: %.1f dB.' % (len(last2), En[-1]-En[0]))
    mono_dec = all(En[i+1] <= En[i]+0.5 for i in range(11))
    P('Class: %s (levels fall %s across the last 12 s; no onsets in the last 2 s means ring-out by the README rule).' % ('ring-out' if len(last2) == 0 else 'fade or other (onsets present in last 2 s)', 'monotonically' if mono_dec else 'non-monotonically'))
    P('')
    P('## Sections'); P('')
    P('Verbatim sections file. Format per line: start time in seconds, the word bar, the 1-based bar number of the start, the section name, and the key in parentheses at the end of the name (some names contain their own parenthesised note, and one may be cut off by the renderer, as printed).'); P('')
    P('```')
    with open(a.sections) as f:
        for l in f: P(l.rstrip('\n'))
    P('```'); P('')
    P('## Tempo map'); P('')
    P('Onset-envelope autocorrelation, 8 s windows, hop 4 s, Viterbi-smoothed pick and the raw strongest peak, pulse strength 0 to 1 (below 0.15 is weak, so the tempo there is low confidence).'); P('')
    P('| window start | window end | smoothed BPM | raw BPM | pulse strength |'); P('|---|---|---|---|---|')
    for w in wins: P('| %.0f | %.0f | %.1f | %.1f | %.2f |' % (w[0], w[1], w[2], w[5], w[3]))
    P('')
    summ = []
    if a.boundary or a.tail:
        P('## Boundary level tables'); P('')
        if old:
            P('Each row pairs second i of the new window with second i of the old window. The largest step is the largest absolute dB difference between neighbouring seconds, reported at the later second.'); P('')
        else:
            P('The largest step is the largest absolute dB difference between neighbouring seconds, reported at the later second.'); P('')
    for ttl, nm, o0, o1 in a.boundary:
        tn = T(nm); ro = (int(o0), int(o1)); span = a.span
        t0n = int(round(tn-span)); t1n = int(round(tn+span))
        a0, sa, stn = tbl(Ln, t0n, t1n)
        P('### '+ttl); P('')
        if old:
            b0, sb, sto = tbl(Lo, ro[0], ro[1])
            P('New boundary at %.2f s (window %d-%d s). Old window %d-%d s.' % (tn, a0, a0+len(sa), b0, b0+len(sb))); P('')
            P('| new t | new dB | old t | old dB |'); P('|---|---|---|---|')
            for i in range(max(len(sa), len(sb))):
                P('| %s | %s | %s | %s |' % (a0+i if i < len(sa) else '', '%.1f' % sa[i] if i < len(sa) else '', b0+i if i < len(sb) else '', '%.1f' % sb[i] if i < len(sb) else ''))
            P('')
            P('Largest step new: %+.1f dB at %d s. Largest step old: %+.1f dB at %d s.' % (stn[1], stn[0], sto[1], sto[0])); P('')
            summ.append((ttl, stn, sto))
        else:
            P('New boundary at %.2f s (window %d-%d s).' % (tn, a0, a0+len(sa))); P('')
            P('| new t | new dB |'); P('|---|---|')
            for i in range(len(sa)): P('| %d | %.1f |' % (a0+i, sa[i]))
            P('')
            P('Largest step new: %+.1f dB at %d s.' % (stn[1], stn[0])); P('')
            summ.append((ttl, stn, None))
    if a.tail:
        ttl, nm = a.tail
        tR = T(nm); later = [s for s in secs if s[0] > tR]
        a0, sa, stn = tbl(Ln, tR, len(Ln))
        P('### '+ttl); P('')
        P('New: %s at %.2f s, %s, end at %.2f s. Rows from %d s to the last full second.' % (nm, tR, ', '.join('%s at %.2f s' % (s[2], s[0]) for s in later), dn, a0)); P('')
        if old:
            ob = len(Lo)-20
            P('Old: the last 20 s only (%d-%d s), no section file is used for the old render.' % (ob, len(Lo))); P('')
            P('| new t | new dB | | old t | old dB |'); P('|---|---|---|---|---|')
            for i in range(len(sa)):
                j = i-(len(sa)-20)
                P('| %d | %.1f | %s | %s | %s |' % (a0+i, sa[i], '', ob+j if j >= 0 else '', '%.1f' % Lo[ob+j] if j >= 0 else ''))
            P('')
            so_seg = Lo[ob:]; d = np.diff(so_seg); k = int(np.argmax(np.abs(d)))
        else:
            P('| new t | new dB |'); P('|---|---|')
            for i in range(len(sa)): P('| %d | %.1f |' % (a0+i, sa[i]))
            P('')
        P('Largest step new over this stretch: %+.1f dB at %d s.' % (stn[1], stn[0]))
        for s in later:
            i = int(round(s[0]))
            seg = Ln[i-3:i+4]; dd = np.diff(seg); kk = int(np.argmax(np.abs(dd)))
            P('Largest step within 3 s of the %s boundary (%.2f s): %+.1f dB at %d s.' % (s[2], s[0], dd[kk], i-3+kk+1))
        if old:
            P('Largest step old over its last 20 s: %+.1f dB at %d s.' % (d[k], ob+k+1)); P('')
            summ.append((ttl, stn, (ob+k+1, d[k])))
        else:
            P('')
            summ.append((ttl, stn, None))
    if summ:
        P('### Summary of largest steps'); P('')
        if old:
            P('| boundary | new step dB | new at s | old step dB | old at s |'); P('|---|---|---|---|---|')
            for t, x, b in summ: P('| %s | %+.1f | %d | %+.1f | %d |' % (t, x[1], x[0], b[1], b[0]))
        else:
            P('| boundary | new step dB | new at s |'); P('|---|---|---|')
            for t, x, b in summ: P('| %s | %+.1f | %d |' % (t, x[1], x[0]))
        P('')
    if old and (a.band_compare or a.onset_compare):
        P('## Comparisons'); P('')
        letter = iter('abcdefghijklmnopqrstuvwxyz')
        for s0, s1, lo, hi, o0, o1 in a.band_compare:
            t0, t1 = T(s0), T(s1); lo = float(lo); hi = float(hi); o0 = float(o0); o1 = float(o1)
            bn = band_share(mn, t0, t1, lo, hi); bo = band_share(mo, o0, o1, lo, hi)
            P('(%s) Share of total spectral energy in %g-%g Hz (8192-point FFT power, mono mix): new "%s" %.1f to %.1f s: %.1f percent. Old render %g-%g s: %.1f percent.' % (next(letter), lo, hi, s0, t0, t1, 100*bn, o0, o1, 100*bo)); P('')
        for s0, s1, o0, o1 in a.onset_compare:
            t0, t1 = T(s0), T(s1); o0 = float(o0); o1 = float(o1)
            c_new = onsets_per_s(onn, t0, t1); c_old = onsets_per_s(ono, o0, o1)
            P('(%s) Onsets per second (README onset-envelope peak picker, depends on its threshold; compare, do not read as a note count). New "%s" %.2f to %.2f s, whole seconds from %d: %s, mean %.2f per s over the section length (%d onsets in %.2f s). Old %g-%g s: %s, mean %.2f per s.' % (next(letter), s0, t0, t1, int(np.ceil(t0)), ' '.join(map(str, c_new)), ((onn >= t0)&(onn < t1)).sum()/(t1-t0), int(((onn >= t0)&(onn < t1)).sum()), t1-t0, o0, o1, ' '.join(map(str, c_old)), ((ono >= o0)&(ono < o1)).sum()/(o1-o0))); P('')
    P('Per-second levels for the whole file: %s (columns t_start_s, rms_dbfs). Spectrogram: %s.' % (os.path.basename(out('_levels.tsv')), os.path.basename(out('.png'))))
    with open(out('.md'), 'w') as f: f.write('\n'.join(md)+'\n')
    size, dpi = plot_render(out('.png'), mn, secs, title, dn)
    print('wrote', out('.md'), out('.png'), out('_levels.tsv'), 'png bytes', size, 'dpi', dpi)

if __name__ == '__main__':
    main()
