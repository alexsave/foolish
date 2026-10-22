'use client';
// Board.tsx - draws the kernel's board and turns pointer gestures into a move: click a piece then
// a square, or drag. Which squares are legal is the kernel's answer (targets); the board knows no
// rule beyond "a pawn reaching the last rank asks which piece", and asks the kernel even that.
import { useCallback, useEffect, useRef, useState } from 'react';

export interface BoardProps {
  board: readonly number[];          // 64 squares, a1 = 0; 0 empty, 1..6 white, 9..14 black
  flipped: boolean;                  // black at the bottom
  lastFrom?: number;
  lastTo?: number;
  checkSq?: number;
  /** null: the board takes no moves */
  targets?: ((from: number) => Set<number>) | null;
  isPromotion?: (from: number, to: number) => boolean;
  onMove?: (from: number, to: number, promo: number) => void;
  /** the color whose pieces may be picked up (0 white, 1 black) */
  mover?: number;
}

const FILES = 'abcdefgh';
const PROMO = [4, 3, 2, 1]; // queen, rook, bishop, knight (chess.h CH_*)

export default function Board(p: BoardProps) {
  const ref = useRef<HTMLDivElement>(null);
  const [sel, setSel] = useState<number | null>(null);
  const [legal, setLegal] = useState<Set<number>>(new Set());
  const [drag, setDrag] = useState<{ from: number; x: number; y: number } | null>(null);
  const [promo, setPromo] = useState<{ from: number; to: number } | null>(null);

  // a new position clears a selection that no longer means anything
  const key = p.board.join(',');
  useEffect(() => { setSel(null); setLegal(new Set()); setPromo(null); }, [key]);

  const squareAt = useCallback((x: number, y: number): number | null => {
    const r = ref.current?.getBoundingClientRect();
    if (!r || x < r.left || y < r.top || x >= r.right || y >= r.bottom) return null;
    const f = Math.floor(((x - r.left) / r.width) * 8), rk = Math.floor(((y - r.top) / r.height) * 8);
    return p.flipped ? rk * 8 + (7 - f) : (7 - rk) * 8 + f;
  }, [p.flipped]);

  const ownPiece = (sq: number) => {
    const v = p.board[sq];
    return v !== 0 && p.mover !== undefined && (v >= 9 ? 1 : 0) === p.mover;
  };

  const pick = (sq: number) => {
    const t = p.targets ? p.targets(sq) : new Set<number>();
    setSel(sq);
    setLegal(t);
  };

  const tryMove = (from: number, to: number): boolean => {
    if (!legal.has(to) || !p.onMove) return false;
    if (p.isPromotion?.(from, to)) { setPromo({ from, to }); return true; }
    p.onMove(from, to, 0);
    setSel(null);
    setLegal(new Set());
    return true;
  };

  const down = (e: React.PointerEvent) => {
    if (!p.targets || e.button !== 0) return;
    const sq = squareAt(e.clientX, e.clientY);
    if (sq === null) return;
    if (sel !== null && sel !== sq && tryMove(sel, sq)) return;
    if (ownPiece(sq)) {
      pick(sq);
      setDrag({ from: sq, x: e.clientX, y: e.clientY });
      (e.target as Element).setPointerCapture?.(e.pointerId);
    } else { setSel(null); setLegal(new Set()); }
  };
  const move = (e: React.PointerEvent) => { if (drag) setDrag({ ...drag, x: e.clientX, y: e.clientY }); };
  const up = (e: React.PointerEvent) => {
    if (!drag) return;
    const sq = squareAt(e.clientX, e.clientY);
    setDrag(null);
    if (sq !== null && sq !== drag.from) tryMove(drag.from, sq);
  };

  const rect = ref.current?.getBoundingClientRect();
  const cells = [];
  for (let i = 0; i < 64; i++) {
    const rk = Math.floor(i / 8), f = i % 8;
    const sq = p.flipped ? rk * 8 + (7 - f) : (7 - rk) * 8 + f;
    const light = ((sq >> 3) + (sq & 7)) % 2 === 1;
    const v = p.board[sq];
    const cls = ['sq', light ? 'l' : 'd'];
    if (sq === p.lastFrom || sq === p.lastTo) cls.push('last');
    if (sq === sel) cls.push('sel');
    if (sq === p.checkSq) cls.push('check');
    if (legal.has(sq)) cls.push(v ? 'cap' : 'dot');
    const dragging = drag && drag.from === sq;
    cells.push(
      <div key={sq} className={cls.join(' ')}>
        {v !== 0 && <div className={`pc p${v}${dragging ? ' lift' : ''}`} />}
        {f === 0 && <span className={`co rk ${light ? 'on-l' : 'on-d'}`}>{(sq >> 3) + 1}</span>}
        {rk === 7 && <span className={`co fl ${light ? 'on-l' : 'on-d'}`}>{FILES[sq & 7]}</span>}
      </div>,
    );
  }

  return (
    <div className="board-wrap">
      <div ref={ref} className="board" onPointerDown={down} onPointerMove={move} onPointerUp={up}
           onContextMenu={(e) => e.preventDefault()}>
        {cells}
        {drag && rect && (
          <div className={`pc ghost p${p.board[drag.from]}`}
               style={{ width: rect.width / 8, height: rect.height / 8, left: drag.x - rect.left - rect.width / 16, top: drag.y - rect.top - rect.height / 16 }} />
        )}
        {promo && (
          <div className="promo" onPointerDown={(e) => e.stopPropagation()}>
            {PROMO.map((t) => (
              <button key={t} className={`pc p${1 + t + (p.mover === 1 ? 8 : 0)}`} aria-label="promote"
                      onClick={() => { p.onMove?.(promo.from, promo.to, t); setPromo(null); setSel(null); setLegal(new Set()); }} />
            ))}
            <button className="promo-x" onClick={() => setPromo(null)}>×</button>
          </div>
        )}
      </div>
    </div>
  );
}
