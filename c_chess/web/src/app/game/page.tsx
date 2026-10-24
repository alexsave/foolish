'use client';
// A game, played or watched: the board, both clocks, the moves (and stepping back through them),
// the offers and the result. Everything shown is the kernel's view.
import { Suspense, useEffect, useMemo, useRef, useState } from 'react';
import Link from 'next/link';
import { useRouter, useSearchParams } from 'next/navigation';
import Board from '@/components/Board';
import {
  CC_EV_START, CC_PLAYING, CW_A_ABORT, CW_A_DRAW, CW_A_DRAW_DECLINE, CW_A_REMATCH, CW_A_REMATCH_DECLINE, CW_A_RESIGN,
} from '@/gen/web_layout';
import type { CvView_Snap } from '@/gen/web_layout';
import { net } from '@/lib/net';
import { tc, useNet } from '@/lib/use';
import { resultLine, score } from '@/lib/words';

function clockText(ms: number): string {
  // floored, as the time a player still has: 9:59.6 reads 9:59, never 10:00
  if (ms < 10000) return (Math.floor(ms / 100) / 10).toFixed(1);
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  const mm = h ? String(m).padStart(2, '0') : String(m);
  return `${h ? h + ':' : ''}${mm}:${String(sec).padStart(2, '0')}`;
}

function Player({ v, color, bottom }: { v: CvView_Snap; color: number; bottom: boolean }) {
  const name = color === 0 ? v.white : v.black;
  const ms = v.clockMs[color];
  const running = v.clockRunning && v.status === CC_PLAYING && v.stm === color;
  const turn = v.status === CC_PLAYING && v.stm === color;
  const d = v.diff[color];
  return (
    <div className={`panel player ${turn ? 'turn' : ''}`} style={{ order: bottom ? 3 : 1 }}>
      <div className="who">
        {name.startsWith('Stockfish-')
          ? <span className="name">Stockfish <span className="muted" style={{ fontWeight: 500 }}>level {name.slice(10)}</span></span>
          : <Link prefetch={false} className="name" href={`/user?name=${encodeURIComponent(name)}`} style={{ color: 'inherit' }}>{name || '...'}</Link>}
        <span className="rt">
          {name.startsWith('Stockfish-') ? 'computer' : v.rating[color] || ''}
          {d !== 0 && <span className={d > 0 ? 'up' : 'down'}> {d > 0 ? '+' : ''}{d}</span>}
        </span>
      </div>
      <div className={`clock ${running ? 'on' : ''} ${ms < 20000 && v.status === CC_PLAYING ? 'low' : ''}`}>{clockText(ms)}</div>
    </div>
  );
}

function Game() {
  const id = Number(useSearchParams().get('id') ?? 0);
  const s = useNet();
  const router = useRouter();
  const [flip, setFlip] = useState(false);
  const [shown, setShown] = useState<number | null>(null);   // null: the live position
  const movesRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!id) return;
    net.open(id);
    return () => net.close();
  }, [id]);

  // the clocks run on the animation frame while the game does
  const v = s.view;
  const live = !!v && v.game === id && v.synced === 1 && v.status === CC_PLAYING && v.clockRunning === 1;
  useEffect(() => {
    if (!live) return;
    let raf = 0;
    const loop = () => { net.tick(); raf = requestAnimationFrame(loop); };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, [live]);

  useEffect(() => {
    const e = net.consume();
    if ((e & CC_EV_START) && v?.startGame && v.startGame !== id) router.push(`/game?id=${v.startGame}`);
  }, [s.events, v, id, router]);

  // a new move returns the board to the live position
  const ply = v?.ply ?? 0;
  useEffect(() => { setShown(null); }, [ply]);
  useEffect(() => { movesRef.current?.scrollTo({ top: movesRef.current.scrollHeight }); }, [ply]);

  const past = useMemo(() => (shown !== null && net.k ? net.k.boardAt(shown) : null), [shown, ply]);

  if (!id) return <p className="muted">No game here.</p>;
  if (!v || v.game !== id || !v.synced) return <p className="muted">Loading the game...</p>;

  const mine = v.color >= 0;
  const flipped = (v.color === 1) !== flip;
  const over = v.status !== CC_PLAYING;
  const top = flipped ? 0 : 1;
  const moves = s.moves;
  const rows = [];
  for (let i = 0; moves && i < moves.n; i += 2) {
    rows.push(
      <div className="mrow" key={i}>
        <span className="n">{i / 2 + 1}.</span>
        <span className={`m ${(shown ?? ply) === i + 1 ? 'cur' : ''}`} onClick={() => setShown(i + 1 === ply ? null : i + 1)}>{moves.moves[i].san}</span>
        {i + 1 < moves.n ? <span className={`m ${(shown ?? ply) === i + 2 ? 'cur' : ''}`} onClick={() => setShown(i + 2 === ply ? null : i + 2)}>{moves.moves[i + 1].san}</span> : <span />}
      </div>,
    );
  }
  const theirOffer = mine && (v.drawOffer & (1 << (v.color ^ 1))) !== 0;
  const myOffer = mine && (v.drawOffer & (1 << v.color)) !== 0;
  const theirRematch = mine && (v.rematchOffer & (1 << (v.color ^ 1))) !== 0;
  const myRematch = mine && (v.rematchOffer & (1 << v.color)) !== 0;
  const act = (a: number) => net.run((k) => k.act(a));
  const step = (to: number) => setShown(to >= ply ? null : Math.max(0, to));
  const b = past ?? v;

  return (
    <div className="game" data-my-turn={v.myTurn}>
      <Board
        board={b.board} flipped={flipped} lastFrom={b.lastFrom} lastTo={b.lastTo} checkSq={b.checkSq}
        mover={mine ? v.color : undefined}
        targets={mine && !over && shown === null ? (from) => net.k!.targets(from, v.myTurn === 0) : null}
        isPromotion={(f, t) => net.k!.isPromotion(f, t)}
        onMove={(f, t, p) => net.run((k) => (v.myTurn ? k.move(f, t, p, performance.now()) : k.premove(f, t, p)))}
        premoveFrom={v.premoveFrom} premoveTo={v.premoveTo}
        onCancelPremove={() => net.run((k) => k.premoveCancel())}
      />
      <div className="side">
        <Player v={v} color={top} bottom={false} />
        <div className="panel" style={{ order: 2 }}>
          <div className="row muted" style={{ padding: '10px 14px', fontSize: 13, justifyContent: 'space-between' }}>
            <span>{tc(v.baseS, v.incS)} {v.rated ? 'Rated' : 'Casual'}</span>
            {!mine && <span>Spectating</span>}
          </div>
          <div className="moves" ref={movesRef}>{rows}</div>
          <div className="nav-ply">
            <button className="btn" onClick={() => step(0)} aria-label="first">«</button>
            <button className="btn" onClick={() => step((shown ?? ply) - 1)} aria-label="back">‹</button>
            <button className="btn" onClick={() => step((shown ?? ply) + 1)} aria-label="forward">›</button>
            <button className="btn" onClick={() => setShown(null)} aria-label="last">»</button>
            <button className="btn" onClick={() => setFlip(!flip)} aria-label="flip">⇅</button>
          </div>
          {over && (
            <div className="result">
              <div className="sc">{score(v.status, v.winner)}</div>
              <div className="muted">{resultLine(v.status, v.winner)}</div>
            </div>
          )}
          {theirOffer && !over && (
            <div className="offer">
              <span>Your opponent offers a draw</span>
              <span className="row">
                <button className="btn primary" onClick={() => act(CW_A_DRAW)}>Accept</button>
                <button className="btn" onClick={() => act(CW_A_DRAW_DECLINE)}>Decline</button>
              </span>
            </div>
          )}
          {theirRematch && over && !v.nextGame && (
            <div className="offer">
              <span>Your opponent wants a rematch</span>
              <span className="row">
                <button className="btn primary" onClick={() => act(CW_A_REMATCH)}>Accept</button>
                <button className="btn" onClick={() => act(CW_A_REMATCH_DECLINE)}>Decline</button>
              </span>
            </div>
          )}
          {mine && (
            <div className="actions">
              {!over && v.ply < 2 && <button className="btn" onClick={() => act(CW_A_ABORT)}>Abort</button>}
              {!over && v.ply >= 2 && <button className="btn" disabled={myOffer} onClick={() => act(CW_A_DRAW)}>{myOffer ? 'Draw offered' : 'Offer draw'}</button>}
              {!over && v.ply >= 2 && <button className="btn warn" onClick={() => act(CW_A_RESIGN)}>Resign</button>}
              {over && !v.nextGame && <button className="btn primary" disabled={myRematch} onClick={() => act(CW_A_REMATCH)}>{myRematch ? 'Rematch offered' : 'Rematch'}</button>}
              {over && v.nextGame !== 0 && <Link prefetch={false} className="btn primary" href={`/game?id=${v.nextGame}`}>Go to the rematch</Link>}
              {over && <Link prefetch={false} className="btn" href="/">New opponent</Link>}
              <a className="btn" href={`/c/pgn/${id}`}>PGN</a>
            </div>
          )}
          {!mine && <div className="actions"><a className="btn" href={`/c/pgn/${id}`}>PGN</a></div>}
        </div>
        <Player v={v} color={top ^ 1} bottom={true} />
      </div>
    </div>
  );
}

export default function Page() {
  return <Suspense><Game /></Suspense>;
}
