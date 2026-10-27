'use client';
// An arena (?id=): its time control, the countdown to its start or its end, Join and Withdraw, and
// the standings pushed to its room as they change. A pairing (CC_EV_START) takes a player to the game.
import { Suspense, useEffect, useState } from 'react';
import Link from 'next/link';
import { useRouter, useSearchParams } from 'next/navigation';
import { CC_ARENA_FINISHED, CC_ARENA_RUNNING, CC_ARENA_UPCOMING, CC_EV_START, CV_ARENA_ROWS, CW_ARENA_IN, CW_ARENA_OUT, CW_ARENA_WITHDRAWN } from '@/gen/web_layout';
import Chat from '@/components/Chat';
import { net } from '@/lib/net';
import { tc, useNet } from '@/lib/use';
import { ARENA_REFUSAL, ARENA_STATUS, SPEED_NAME, clockTime, countdown } from '@/lib/words';

/** The mark of a player on a streak: a win scores double. */
function Fire() {
  return (
    <svg className="fire" viewBox="0 0 16 16" aria-label="on fire: a win scores double" role="img">
      <path d="M8 1c.6 2.6 3.9 4.2 3.9 8.1A3.9 3.9 0 0 1 8 13a3.9 3.9 0 0 1-3.9-3.9c0-1.7.8-2.8 1.7-3.6.1 1.2.7 2 1.5 2.3C7 5.6 7.1 3.1 8 1z" />
    </svg>
  );
}

function Arena() {
  const id = Number(useSearchParams().get('id') ?? 0);
  const s = useNet();
  const router = useRouter();
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);
  const [missing, setMissing] = useState(false);

  useEffect(() => {
    if (!id) return;
    net.arena(id);
    void net.arenaLoad(id).then((st) => setMissing(st === 404));
    return () => net.arena(0);
  }, [id]);
  useEffect(() => { const t = setInterval(() => net.tickArena(), 1000); return () => clearInterval(t); }, []);
  useEffect(() => {
    const e = net.consume();
    if ((e & CC_EV_START) && s.view?.startGame) router.push(`/game?id=${s.view.startGame}`);
  }, [s.events, s.view, router]);

  if (!id || missing) return <p className="muted">No such arena. <Link prefetch={false} href="/arenas">All arenas</Link></p>;
  const a = s.arena;
  if (!a || !a.ok || a.id !== id) return <p className="muted">Loading the arena...</p>;
  const act = async (join: boolean) => {
    setBusy(true);
    setErr('');
    const r = await net.arenaAct(id, join);
    setBusy(false);
    if (r.status !== 200) setErr(ARENA_REFUSAL[r.code] ?? `Refused (${r.status}).`);
  };
  const over = a.status === CC_ARENA_FINISHED;
  const rows = a.rows.slice(0, a.n);
  return (
    <div className="arena">
      <section className="panel arena-head">
        <div className="row" style={{ justifyContent: 'space-between', alignItems: 'flex-start' }}>
          <div>
            <h1>{a.name}</h1>
            <div className="muted">
              {tc(a.baseS, a.incS)} {SPEED_NAME[a.speed]}, {a.rated ? 'rated' : 'casual'}
              {a.by ? <>, made by <Link prefetch={false} href={`/user?name=${encodeURIComponent(a.by)}`}>{a.by}</Link></> : ', every hour'}
            </div>
          </div>
          <div className="arena-clock">
            <span className="k">{a.status === CC_ARENA_UPCOMING ? 'Starts in' : a.status === CC_ARENA_RUNNING ? 'Ends in' : ARENA_STATUS[a.status]}</span>
            <span className={`t ${over ? 'at' : ''}`}>{a.status === CC_ARENA_UPCOMING ? countdown(a.startsInMs) : a.status === CC_ARENA_RUNNING ? countdown(a.endsInMs) : `at ${clockTime(a.endsAt)}`}</span>
          </div>
        </div>
        <div className="stats" style={{ padding: '14px 0 0' }}>
          <span><b>{a.nPlayers}</b> {a.nPlayers === 1 ? 'player' : 'players'}</span>
          <span><b>{a.playing}</b> playing</span>
          <span><b>{a.games}</b> {a.games === 1 ? 'game' : 'games'}</span>
          {a.status === CC_ARENA_UPCOMING && <span>from {clockTime(a.startsAt)} to {clockTime(a.endsAt)}</span>}
        </div>
        {over && rows.length > 0 && (
          <p style={{ margin: '14px 0 0' }}>Won by <Link prefetch={false} href={`/user?name=${encodeURIComponent(rows[0].name)}`}><b>{rows[0].name}</b></Link> with {rows[0].score} points from {rows[0].games} games.</p>
        )}
        {(!over || a.meIn !== CW_ARENA_OUT || err) && <div className="row arena-actions">
          {!over && a.meIn !== CW_ARENA_IN && <button className="btn primary" disabled={busy} onClick={() => void act(true)}>{a.meIn === CW_ARENA_WITHDRAWN ? 'Join again' : 'Join'}</button>}
          {!over && a.meIn === CW_ARENA_IN && <button className="btn" disabled={busy} onClick={() => void act(false)}>Withdraw</button>}
          {a.meIn !== CW_ARENA_OUT && a.meRank > 0 && (
            <span className="muted">
              You: <b className="ink">#{a.meRank}</b> with {a.meScore} {a.meScore === 1 ? 'point' : 'points'} from {a.meGames} {a.meGames === 1 ? 'game' : 'games'}
              {a.meFire ? ', on fire' : ''}{a.meIn === CW_ARENA_WITHDRAWN ? ', withdrawn' : ''}
              {!a.meListed && a.n >= CV_ARENA_ROWS ? ` (the standings list the top ${CV_ARENA_ROWS})` : ''}
            </span>
          )}
          {err && <span className="err">{err}</span>}
        </div>}
        {a.meIn === CW_ARENA_IN && a.status === CC_ARENA_UPCOMING && <p className="muted hint">You are in. Your first game starts with the arena.</p>}
        {a.meIn === CW_ARENA_IN && a.status === CC_ARENA_RUNNING && <p className="muted hint">Stay on this page: your next game opens by itself.</p>}
      </section>
      <section className="panel"><Chat room={`a${id}`} title="Arena chat" /></section>
      <section className="panel">
        <table className="seeks standings">
          <thead><tr><th>#</th><th>Player</th><th>Rating</th><th>Games</th><th>Score</th></tr></thead>
          <tbody>
            {rows.map((r) => (
              <tr key={r.name} className={`${r.me ? 'mine' : ''} ${r.withdrawn ? 'out' : ''}`}>
                <td className="muted">{r.rank}</td>
                <td>
                  <Link prefetch={false} href={`/user?name=${encodeURIComponent(r.name)}`}>{r.name}</Link>
                  {r.playing ? <span className="live-dot" title="playing" aria-label="playing" /> : null}
                  {r.withdrawn ? <span className="tag">withdrawn</span> : null}
                </td>
                <td className="muted">{r.rating}</td>
                <td className="muted">{r.games}</td>
                <td className="score">{r.fire ? <Fire /> : null}{r.score}</td>
              </tr>
            ))}
            {!rows.length && <tr><td colSpan={5} className="muted" style={{ padding: 24, textAlign: 'center' }}>Nobody has joined yet.</td></tr>}
          </tbody>
        </table>
      </section>
      <p className="muted" style={{ fontSize: 13 }}><Link prefetch={false} href="/arenas">All arenas</Link></p>
    </div>
  );
}

export default function Page() { return <Suspense><Arena /></Suspense>; }
