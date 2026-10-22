// MiniBoard.tsx - a game in play as the lobby and the watch page show it: a small board, the names,
// ratings and clocks; a link to the game.
import Link from 'next/link';
import type { CvTvGame_Snap } from '@/gen/web_layout';

function clock(ms: number): string {
  const s = Math.ceil(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

export default function MiniBoard({ g }: { g: CvTvGame_Snap }) {
  const cells = [];
  for (let i = 0; i < 64; i++) {
    const sq = (7 - Math.floor(i / 8)) * 8 + (i % 8);
    const light = ((sq >> 3) + (sq & 7)) % 2 === 1;
    const v = g.board[sq];
    const last = sq === g.lastFrom || sq === g.lastTo;
    cells.push(<div key={sq} className={`sq ${light ? 'l' : 'd'}${last ? ' last' : ''}`}>{v !== 0 && <div className={`pc p${v}`} />}</div>);
  }
  const side = (c: number) => (
    <div className="mini-who">
      <span className="mini-name">{c === 0 ? g.white : g.black} <span className="muted">{g.rating[c]}</span></span>
      <span className={`mini-clock ${g.clockRunning && g.stm === c ? 'on' : ''}`}>{clock(g.clockMs[c])}</span>
    </div>
  );
  return (
    <Link prefetch={false} href={`/game?id=${g.game}`} className="mini">
      {side(1)}
      <div className="board mini-board">{cells}</div>
      {side(0)}
    </Link>
  );
}
