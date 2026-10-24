'use client';
// puzzles/page.tsx - one puzzle after another near your puzzle rating. The kernel solves on its
// own board (a wrong move ends it, any mate counts); the server hears once how it went and moves
// both ratings.
import { useCallback, useEffect, useRef, useState } from 'react';
import Board from '@/components/Board';
import { net } from '@/lib/net';
import { useNet } from '@/lib/use';
import { CP_FAILED, CP_SOLVED, CP_SOLVING, CP_T_MATE } from '@/gen/web_layout';
import type { CvPuzzle_Snap } from '@/gen/web_layout';

const INTRO_MS = 600;
const REPLY_MS = 350;
const SHOW_MS = 550;

export default function Puzzles() {
  useNet();   // the socket comes up with the page
  const [pz, setPz] = useState<CvPuzzle_Snap | null>(null);
  const [none, setNone] = useState(false);
  const [right, setRight] = useState(false);   // the last move was right and the line goes on
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const later = useCallback((fn: () => void, ms: number) => { if (timer.current) clearTimeout(timer.current); timer.current = setTimeout(fn, ms); }, []);
  useEffect(() => () => { if (timer.current) clearTimeout(timer.current); }, []);

  const next = useCallback(async () => {
    setRight(false);
    const r = await net.request((k, tag) => k.puzzleAsk(tag));
    if (r.status !== 200) { setNone(true); setPz(null); return; }
    setNone(false);
    setPz(net.k!.puzzleLoad(r.body));
    later(() => setPz(net.k!.puzzleBegin()), INTRO_MS);
  }, [later]);
  useEffect(() => { void next(); }, [next]);

  const report = async (win: boolean) => {
    const r = await net.request((k, tag) => k.puzzleSend(win, tag));
    if (r.status === 200) setPz(net.k!.puzzleDone(r.body));
  };

  const onMove = (f: number, t: number, promo: number) => {
    const k = net.k!;
    const after = k.puzzleTry(f, t, promo);
    setPz(after);
    if (after.state === CP_FAILED) { setRight(false); void report(false); return; }
    if (after.state === CP_SOLVED && !after.replyPending) { setRight(false); void report(true); return; }
    setRight(true);
    if (after.replyPending) later(() => {
      const p = k.puzzleReply();
      setPz(p);
      if (p.state === CP_SOLVED) { setRight(false); void report(true); }
    }, REPLY_MS);
  };

  const showLine = () => {
    const k = net.k!;
    const step = () => { if (k.puzzleReveal()) { setPz(k.puzzle()); later(step, SHOW_MS); } };
    step();
  };

  if (none) return <div className="panel empty">No puzzles on this server yet.</div>;
  if (!pz || !pz.ok) return <div className="panel empty muted">Finding a puzzle...</div>;

  const side = pz.color === 0 ? 'White' : 'Black';
  const solving = pz.state === CP_SOLVING;
  const delta = pz.done ? pz.after - pz.before : 0;
  const shownAll = pz.at >= pz.n;
  return (
    <div className="game puzzle">
      <div className="board-row">
        <Board
          board={pz.board} flipped={pz.color === 1} lastFrom={pz.lastFrom} lastTo={pz.lastTo} checkSq={pz.checkSq} wrongSq={pz.wrongSq}
          mover={pz.color}
          targets={solving && !pz.replyPending && !pz.intro ? (from) => net.k!.puzzleTargets(from) : null}
          isPromotion={(f, t) => net.k!.puzzleIsPromotion(f, t)}
          onMove={onMove}
        />
      </div>
      <div className="side">
        <div className="panel pz">
          {solving && (
            <div className="pz-say">
              <span className={`pz-dot ${pz.color === 0 ? 'w' : 'b'}`} />
              <div>
                <b>{right ? 'Best move!' : pz.intro ? 'Get ready' : 'Your turn'}</b>
                <div className="muted">{right ? 'Keep going...' : `Find the best move for ${side}.`}</div>
              </div>
            </div>
          )}
          {pz.state === CP_SOLVED && (
            <div className="pz-say ok">
              <span className="pz-mark">✓</span>
              <div><b>Success!</b><div className="muted">{pz.themes & CP_T_MATE ? 'Mate found.' : 'Puzzle solved.'}</div></div>
            </div>
          )}
          {pz.state === CP_FAILED && (
            <div className="pz-say bad">
              <span className="pz-mark">✗</span>
              <div><b>That is not the move</b><div className="muted">{shownAll ? 'That was the line.' : 'The puzzle counts as failed.'}</div></div>
            </div>
          )}
          <div className="pz-rows">
            <div className="row"><span className="muted">Your puzzle rating</span><b>{pz.rating}{pz.done ? <span className={delta >= 0 ? 'up' : 'down'}> {delta >= 0 ? '+' : ''}{delta}</span> : null}</b></div>
            <div className="row"><span className="muted">Solved</span><b>{pz.solved} of {pz.tried}</b></div>
            {pz.done ? <div className="row"><span className="muted">This puzzle</span><b>{pz.puzzleRating}</b></div> : null}
          </div>
          {!solving && (
            <div className="actions">
              {pz.state === CP_FAILED && !shownAll && <button className="btn" onClick={showLine}>Show the solution</button>}
              <button className="btn primary" onClick={() => void next()}>Next puzzle</button>
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
