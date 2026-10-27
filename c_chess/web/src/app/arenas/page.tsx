'use client';
// The arenas: running, upcoming and just finished (GET /c/arenas, read by the kernel), and a form to
// make one for a signed-in player.
import { useState } from 'react';
import Link from 'next/link';
import { useRouter } from 'next/navigation';
import type { CvArenaItem_Snap } from '@/gen/web_layout';
import { CC_ARENA_DURATION_MIN, CC_ARENA_FINISHED, CC_ARENA_RUNNING, CC_ARENA_UPCOMING } from '@/gen/web_layout';
import { useArenas } from '@/lib/arenas';
import { net } from '@/lib/net';
import { tc, useNet } from '@/lib/use';
import { ARENA_REFUSAL, SPEED_NAME, clockTime, inWords } from '@/lib/words';

const TCS: [number, number][] = [[60, 0], [180, 0], [180, 2], [300, 0], [600, 0], [900, 10]];
const LENGTHS = [20, 30, 45, 60, 90];   // minutes
const STARTS = [0, 5, 15, 30, 60];      // minutes from now

function Rows({ rows, status }: { rows: CvArenaItem_Snap[]; status: number }) {
  const router = useRouter();
  if (!rows.length) return <p className="muted empty">{status === CC_ARENA_RUNNING ? 'None running right now.' : status === CC_ARENA_UPCOMING ? 'None coming up.' : 'None finished in the last hour.'}</p>;
  return (
    <table className="seeks arenas">
      <thead><tr><th>Arena</th><th>Time</th><th>Players</th><th>{status === CC_ARENA_FINISHED ? 'Winner' : status === CC_ARENA_RUNNING ? 'Ends' : 'Starts'}</th></tr></thead>
      <tbody>
        {rows.map((a) => (
          <tr key={a.id} className="click" onClick={() => router.push(`/arena?id=${a.id}`)}>
            <td><Link prefetch={false} href={`/arena?id=${a.id}`}>{a.name}</Link></td>
            <td>{tc(a.baseS, a.incS)} <span className="muted">{SPEED_NAME[a.speed]}{a.rated ? '' : ', casual'}</span></td>
            <td>{a.nPlayers}</td>
            <td className="muted">{status === CC_ARENA_FINISHED ? (a.winner || '-') : status === CC_ARENA_RUNNING ? inWords(a.endsInMs) : `${clockTime(a.startsAt)}, ${inWords(a.startsInMs)}`}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

function Create() {
  const router = useRouter();
  const me = useNet().view?.me ?? '';
  const [t, setT] = useState(1);
  const [len, setLen] = useState(30);
  const [start, setStart] = useState(5);
  const [rated, setRated] = useState(true);
  const [name, setName] = useState('');
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);
  if (!me) return null;
  if (me.startsWith('guest-')) {
    return <p className="muted" style={{ margin: 0 }}>Anyone may play an arena. To make one, <Link prefetch={false} href="/account">sign in</Link>.</p>;
  }
  const [b, i] = TCS[t];
  const nm = name || `${me} arena`;
  const ok = !!net.k && net.k.arenaNameOk(nm);
  const create = async () => {
    setBusy(true);
    setErr('');
    const r = await net.request((k, tag) => k.arenaCreate(b, i, rated, start * 60, Math.max(CC_ARENA_DURATION_MIN, len * 60), nm, tag));
    const a = net.k!.arenaRead(r.body, performance.now());
    setBusy(false);
    if (a.ok) router.push(`/arena?id=${a.id}`);
    else setErr(ARENA_REFUSAL[a.err] ?? `Refused (${r.status}).`);
  };
  return (
    <div className="arena-form">
      <label>Name<input value={name} maxLength={30} placeholder={`${me} arena`} onChange={(e) => setName(e.target.value)} /></label>
      <div className="arena-opts">
        <span className="muted">Time</span>
        <div className="seg">{TCS.map(([x, y], k) => <button key={k} className={k === t ? 'on' : ''} onClick={() => setT(k)}>{tc(x, y)}</button>)}</div>
        <span className="muted">Length</span>
        <div className="seg">{LENGTHS.map((m) => <button key={m} className={m === len ? 'on' : ''} onClick={() => setLen(m)}>{m} min</button>)}</div>
        <span className="muted">Starts</span>
        <div className="seg">{STARTS.map((m) => <button key={m} className={m === start ? 'on' : ''} onClick={() => setStart(m)}>{m ? `in ${m}` : 'now'}</button>)}</div>
        <span className="muted">Mode</span>
        <div className="seg">
          <button className={rated ? 'on' : ''} onClick={() => setRated(true)}>Rated</button>
          <button className={!rated ? 'on' : ''} onClick={() => setRated(false)}>Casual</button>
        </div>
      </div>
      <div className="row">
        <button className="btn primary" disabled={!ok || busy} onClick={() => void create()}>Create arena</button>
        {name && !ok && <span className="err">3 to 30 letters, digits, spaces, - and _</span>}
        {err && <span className="err">{err}</span>}
      </div>
    </div>
  );
}

export default function Arenas() {
  const list = useArenas();
  const rows = list ? list.rows.slice(0, list.n) : [];
  const of = (st: number) => rows.filter((a) => a.status === st);
  return (
    <div className="arenas-page">
      <section className="panel">
        <h1 className="panel-h">Arenas</h1>
        <p className="muted panel-p">
          Join, and the arena pairs you with another free player near your rating as soon as your game ends, until its time runs out.
          A win scores 2, a draw 1; after two wins in a row you are on fire and score double until you lose or draw.
        </p>
        <h2 className="panel-h">Running</h2>
        {list ? <Rows rows={of(CC_ARENA_RUNNING)} status={CC_ARENA_RUNNING} /> : <p className="muted empty">Loading...</p>}
        <h2 className="panel-h">Upcoming</h2>
        {list && <Rows rows={of(CC_ARENA_UPCOMING)} status={CC_ARENA_UPCOMING} />}
        <h2 className="panel-h">Finished</h2>
        {list && <Rows rows={of(CC_ARENA_FINISHED)} status={CC_ARENA_FINISHED} />}
      </section>
      <aside className="panel" style={{ padding: 16 }}>
        <h2>Make an arena</h2>
        <Create />
      </aside>
    </div>
  );
}
