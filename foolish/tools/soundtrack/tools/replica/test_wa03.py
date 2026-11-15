"""test_wa03.py: tests of the wa03 recreation tools on synthetic audio.  Run with the venv python:  python test_wa03.py
Mutation checks (each break, the test that went red) are in README.md under 'wa03'."""
import os, sys, subprocess, tempfile, importlib.util
import numpy as np, soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
SR = 22050
T = tempfile.mkdtemp()


def tone(m, t0, dur, amp, n, delay=0.0):
    f = 440 * 2 ** ((m - 69) / 12.0); y = np.zeros(n); t = np.arange(int(dur * SR)) / SR
    env = np.minimum(1, t / 0.004) * np.exp(-t * 2)
    s = sum(amp / h * np.sin(2 * np.pi * f * h * t) for h in (1, 2, 3)) * env
    i = int((t0 + delay) * SR); y[i:i + len(s)] += s[:n - i]
    return y


def run(script, *args):
    r = subprocess.run([sys.executable, os.path.join(HERE, script)] + list(args), capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout


def read(p):
    return [l.split('\t') for l in open(p) if l.strip()]


def test_events():
    spec = importlib.util.spec_from_file_location('ev', os.path.join(HERE, 'wa03_events.py')); ev = importlib.util.module_from_spec(spec); spec.loader.exec_module(ev)
    notes = [('piano', 1.0, 2.5, 60.0, 0.5)] + [('cello', 0.25 * i, 0.25 * i + 0.2, 50.0, 0.4) for i in range(45)]
    txt = ev.to_score(notes)
    assert 'TEMPO 120' in txt
    assert 'EVENTS PIANO: 2.000000,60.000,3.000000,0.5000' in txt, 'beat must be exactly 2 * seconds'
    assert txt.count('EVENTS CELLO:') == 2, 'at most 40 events per line'


def test_vel():
    n = int(6 * SR)    # both renders are scaled to the same total, so only the balance between notes can be tested: note B is 12 dB too quiet
    orig = tone(60, 1.0, 1.5, 0.5, n) + tone(67, 3.5, 1.5, 0.5, n); rep = tone(60, 1.0, 1.5, 0.5, n) + tone(67, 3.5, 1.5, 0.125, n)
    sf.write(T + '/o.wav', orig, SR); sf.write(T + '/r.wav', rep, SR)
    open(T + '/n.tsv', 'w').write('piano\t1.0\t2.5\t60\t0.5\npiano\t3.5\t5.0\t67\t0.5\n')
    run('wa03_vel.py', T + '/n.tsv', T + '/o.wav', T + '/r.wav', T + '/o.tsv')
    r = read(T + '/o.tsv'); va, vb = float(r[0][4]), float(r[1][4])
    assert vb > va + 0.1, 'the too quiet note must end above the right one: %.3f %.3f' % (va, vb)


def test_timing():
    n = int(4 * SR)
    orig = tone(64, 1.0, 1.2, 0.5, n) + tone(67, 2.2, 1.2, 0.5, n); rep = tone(64, 1.0, 1.2, 0.5, n, 0.020) + tone(67, 2.2, 1.2, 0.5, n)   # note 1 is 20 ms late in the replica
    sf.write(T + '/o.wav', orig, SR); sf.write(T + '/r.wav', rep, SR)
    open(T + '/n.tsv', 'w').write('piano\t1.02\t2.0\t64\t0.5\npiano\t2.2\t3.0\t67\t0.5\n')
    run('wa03_timing.py', T + '/n.tsv', T + '/o.wav', T + '/r.wav', T + '/o.tsv')
    rows = read(T + '/o.tsv'); s0 = float(rows[0][1])
    assert s0 < 1.02 - 0.008, 'the late note must move earlier, got %.4f' % s0
    assert abs(float(rows[1][1]) - 2.2) < 0.012, 'the on-time note must stay'


def test_resid():
    n = int(5 * SR)
    orig = tone(60, 1.0, 3.0, 0.5, n) * 0 + sum(0.4 / h * np.sin(2 * np.pi * 261.63 * h * np.arange(n) / SR) for h in (1, 2, 3)) * (np.arange(n) > SR) * (np.arange(n) < 4 * SR)
    sf.write(T + '/o.wav', orig, SR); sf.write(T + '/r.wav', np.zeros(n) + 1e-5 * np.random.RandomState(1).randn(n), SR)
    open(T + '/n.tsv', 'w').write('piano\t0.1\t0.3\t72\t0.5\n')
    out = run('wa03_resid.py', T + '/n.tsv', T + '/o.wav', T + '/r.wav', T + '/o.tsv', '--db', '9')
    rows = read(T + '/o.tsv')
    added = [r for r in rows if abs(float(r[3]) - 60) < 0.5]
    assert added and abs(float(added[0][1]) - 1.0) < 0.15, 'the sounding C4 the replica lacks must be added at about 1.0 s: %s' % out
    assert float(added[0][2]) - float(added[0][1]) > 2.0


def test_post_pedal():
    n = int(8 * SR)
    y = tone(40, 0.2, 3.5, 0.8, n) + tone(45, 4.0, 3.5, 0.8, n)    # a bass change at 4.0 s
    sf.write(T + '/o.wav', y, SR)
    open(T + '/n.tsv', 'w').write('piano\t4.0\t5.0\t45\t0.5\n')
    out = run('wa03_post.py', T + '/n.tsv', T + '/o.wav', T + '/o.tsv', '--no-refine', '--min-gap-s', '1.5', '--chroma-thr', '90')
    assert 'pedal lifts at' in out and any(abs(float(x) - 4.0) < 0.15 for x in out.split('at')[1].split()), out


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_') and (len(sys.argv) < 2 or sys.argv[1] in name):
            fn(); print('ok', name)
