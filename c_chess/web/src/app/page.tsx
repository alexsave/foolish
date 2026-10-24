'use client';
// The lobby: quick pairing by time control, the open seeks, a challenge link.
import { useEffect, useState } from 'react';
import Link from 'next/link';
import { useRouter } from 'next/navigation';
import { CC_EV_CHALLENGE, CC_EV_START } from '@/gen/web_layout';
import { net } from '@/lib/net';
import { tc, useNet } from '@/lib/use';
import MiniBoard from '@/components/MiniBoard';
import Leaders from '@/components/Leaders';

const QUICK: [number, number, string][] = [
  [60, 0, 'Bullet'], [120, 1, 'Bullet'], [180, 0, 'Blitz'],
  [180, 2, 'Blitz'], [300, 0, 'Blitz'], [300, 3, 'Blitz'],
  [600, 0, 'Rapid'], [600, 5, 'Rapid'], [900, 10, 'Rapid'],
];

export default function Lobby() {
  const s = useNet();
  const router = useRouter();
  const [waiting, setWaiting] = useState<string | null>(null);
  const [rated, setRated] = useState(true);
  const [link, setLink] = useState<string | null>(null);
  const [level, setLevel] = useState(3);
  const [side, setSide] = useState(2);   // 0 white, 1 black, 2 either

  useEffect(() => { net.lobby(true); return () => net.lobby(false); }, []);
  // the live games' clocks tick between the server's once-a-second updates
  useEffect(() => { const t = setInterval(() => net.tickTv(), 1000); return () => clearInterval(t); }, []);
  useEffect(() => {
    const e = net.consume();
    if ((e & CC_EV_START) && s.view?.startGame) router.push(`/game?id=${s.view.startGame}`);
    if ((e & CC_EV_CHALLENGE) && s.view?.challengeId) setLink(`${location.origin}/join?c=${s.view.challengeId}`);
  }, [s.events, s.view, router]);
  // a waiting seek is sent again before the server's 30 s are up
  useEffect(() => {
    if (!waiting) return;
    const [b, i] = waiting.split('+').map(Number);
    const t = setInterval(() => net.run((k) => k.seek(b, i, rated)), 20000);
    return () => clearInterval(t);
  }, [waiting, rated]);

  const seek = (b: number, i: number) => {
    const key = `${b}+${i}`;
    if (waiting === key) { setWaiting(null); net.run((k) => k.unseek()); return; }
    setWaiting(key);
    net.run((k) => k.seek(b, i, rated));
  };
  const lobby = s.lobby;
  const me = s.view?.me;
  return (
    <div className="lobby">
      <section>
        <div className="row" style={{ justifyContent: 'space-between', marginBottom: 12 }}>
          <h1>Quick pairing</h1>
          <div className="seg" role="radiogroup" aria-label="mode">
            <button role="radio" aria-checked={rated} className={rated ? 'on' : ''} onClick={() => setRated(true)}>Rated</button>
            <button role="radio" aria-checked={!rated} className={!rated ? 'on' : ''} onClick={() => setRated(false)}>Casual</button>
          </div>
        </div>
        <div className="pairs">
          {QUICK.map(([b, i, k]) => {
            const key = `${b}+${i}`;
            return (
              <button key={key} className={`pair ${waiting === key ? 'waiting' : ''}`} onClick={() => seek(b, i)}>
                <span className="t">{tc(b, i)}</span>
                <span className="k">{waiting === key ? 'Waiting for an opponent...' : k}</span>
              </button>
            );
          })}
        </div>
        <div className="panel" style={{ marginTop: 24, padding: 16 }}>
          <h2>Play a friend</h2>
          <div className="row">
            <span className="muted" style={{ fontSize: 14 }}>A link anyone can open to play you, 5+3 casual.</span>
            <button className="btn" style={{ marginLeft: 'auto' }} onClick={() => net.run((k) => k.challenge(300, 3, false))}>Create link</button>
          </div>
          {link && (
            <div className="link-box">
              <input readOnly value={link} onFocus={(e) => e.target.select()} />
              <button className="btn primary" onClick={() => navigator.clipboard?.writeText(link)}>Copy</button>
            </div>
          )}
        </div>
        <div className="panel" style={{ marginTop: 16, padding: 16 }}>
          <div className="row" style={{ justifyContent: 'space-between' }}>
            <h2 style={{ margin: 0 }}>Play the computer</h2>
            <div className="seg" role="radiogroup" aria-label="your color">
              {['White', 'Either', 'Black'].map((t, i) => {
                const v = i === 0 ? 0 : i === 1 ? 2 : 1;
                return <button key={t} role="radio" aria-checked={side === v} className={side === v ? 'on' : ''} onClick={() => setSide(v)}>{t}</button>;
              })}
            </div>
          </div>
          <div className="row" style={{ marginTop: 12, gap: 8 }}>
            <div className="levels" role="radiogroup" aria-label="level">
              {[1, 2, 3, 4, 5, 6, 7, 8].map((l) => (
                <button key={l} role="radio" aria-checked={level === l} className={`lvl ${level === l ? 'on' : ''}`} onClick={() => setLevel(l)}>{l}</button>
              ))}
            </div>
            <button className="btn primary" style={{ marginLeft: 'auto' }} onClick={() => net.run((k) => k.ai(600, 0, level, side))}>Play level {level}, 10+0</button>
          </div>
        </div>
        <div className="row" style={{ justifyContent: 'space-between', margin: '28px 0 12px' }}>
          <h2 style={{ margin: 0 }}>Live games</h2>
          <Link prefetch={false} href="/watch" className="muted" style={{ fontSize: 14 }}>All games</Link>
        </div>
        {s.tv?.n ? (
          <div className="minis">{s.tv.games.slice(0, Math.min(6, s.tv.n)).map((g) => <MiniBoard key={g.game} g={g} />)}</div>
        ) : (
          <div className="panel muted" style={{ padding: 24, textAlign: 'center' }}>No games in play right now.</div>
        )}
      </section>
      <aside className="panel">
        <div className="stats">
          <span><b>{lobby?.games ?? 0}</b> {lobby?.games === 1 ? 'game' : 'games'} in play</span>
          <span><b>{lobby?.n ?? 0}</b> {lobby?.n === 1 ? 'seek' : 'seeks'} waiting</span>
          <span><b>{lobby?.players ?? 0}</b> players</span>
        </div>
        <table className="seeks">
          <thead><tr><th>Player</th><th>Rating</th><th>Time</th><th>Mode</th></tr></thead>
          <tbody>
            {(lobby?.seeks ?? []).slice(0, lobby?.n ?? 0).map((r) => (
              <tr key={r.id} className={r.name === me ? '' : 'click'} onClick={() => r.name !== me && net.run((k) => k.seek(r.baseS, r.incS, r.rated === 1))}>
                <td>{r.name}</td><td>{r.rating}</td><td>{tc(r.baseS, r.incS)}</td><td>{r.rated ? 'Rated' : 'Casual'}</td>
              </tr>
            ))}
            {!lobby?.n && <tr><td colSpan={4} className="muted" style={{ padding: 24, textAlign: 'center' }}>No one is waiting. Pick a time to be first.</td></tr>}
          </tbody>
        </table>
        <Leaders limit={10} />
      </aside>
    </div>
  );
}
