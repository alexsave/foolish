'use client';

// The tournament's bracket (wire.h be_w_tour, tournament.h): six rounds of matches left to right,
// the scores as the server's rated games decide them, the champion at the end. Positions are the
// bracket's own (a match sits between the two it is fed by); this file only draws.

import { useLayoutEffect, useRef, useState } from 'react';
import type { WBot, WTour } from '../api';
import { BE_TOUR_DONE, BE_TOUR_NONE, BE_TOUR_RUNNING, BE_TOUR_WIN } from '../gen/wire';
import { botName, familyColor } from './League';

const ROUNDS = ['Opening round', 'Round of 32', 'Round of 16', 'Quarterfinals', 'Semifinals', 'Final'];
const ROUND_SIZE = [16, 16, 8, 4, 2, 1];
const ROUND_AT = [0, 16, 32, 40, 44, 46];
const BOX_H = 38;
const HEAD_H = 28;

interface Props {
  tour: WTour;
  bots: WBot[];
}

// Where a match's box sits: its row center, in units of the opening round's rows (0..16).
function center(round: number, slot: number): number {
  if (round <= 1) return slot + 0.5;
  const span = 2 ** (round - 1);
  return slot * span + span / 2;
}

function Side({ bot, seed, wins, won, lost, bots }: { bot: number; seed: number; wins: number; won: boolean; lost: boolean; bots: WBot[] }) {
  const b = bot === BE_TOUR_NONE ? null : bots[bot];
  return (
    <div className={`bk-side${won ? ' won' : ''}${lost ? ' lost' : ''}`}>
      <span className="bk-seed num">{seed || ''}</span>
      {b ? <span className="fam-dot" style={{ background: familyColor(b.key) }} /> : <span className="fam-dot empty" />}
      <span className="bk-name">{b ? botName(b) : <span className="faint">to be decided</span>}</span>
      <span className="bk-wins num">{b ? wins : ''}</span>
    </div>
  );
}

export function Bracket({ tour, bots }: Props) {
  const wrap = useRef<HTMLDivElement>(null);
  const [size, setSize] = useState({ w: 1200, h: 720 });
  useLayoutEffect(() => {
    const el = wrap.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setSize({ w: el.clientWidth, h: el.clientHeight }));
    ro.observe(el);
    setSize({ w: el.clientWidth, h: el.clientHeight });
    return () => ro.disconnect();
  }, []);

  const cols = 7; // six rounds and the champion
  const gap = 22;
  const colW = Math.max(150, (size.w - gap * (cols - 1)) / cols);
  const rowH = Math.max(BOX_H + 2, (size.h - HEAD_H) / 16);
  const x = (r: number) => r * (colW + gap);
  const y = (r: number, s: number) => HEAD_H + center(r, s) * rowH;

  // The lines from each match to the one it feeds.
  const lines: string[] = [];
  for (let r = 0; r < 5; r++)
    for (let s = 0; s < ROUND_SIZE[r]; s++) {
      const nr = r + 1;
      const ns = r === 0 ? s : Math.floor(s / 2);
      const x0 = x(r) + colW, y0 = y(r, s);
      const x1 = x(nr), y1 = y(nr, ns) + (r === 0 ? BOX_H / 4 : (s % 2 === 0 ? -1 : 1) * (BOX_H / 4));
      const xm = x0 + gap / 2;
      lines.push(`M${x0},${y0} H${xm} V${y1} H${x1}`);
    }
  const final = tour.matches[ROUND_AT[5]];
  const champ = tour.champion !== BE_TOUR_NONE ? bots[tour.champion] : null;
  const champSeed = champ ? (final?.winner === final?.a ? final?.seedA : final?.seedB) : 0;

  return (
    <div className="bracket" ref={wrap}>
      <svg className="bk-lines" width={size.w} height={size.h} aria-hidden="true">
        {lines.map((d, i) => (
          <path key={i} d={d} />
        ))}
        <path d={`M${x(5) + colW},${y(5, 0)} H${x(6)}`} />
      </svg>
      {ROUNDS.map((name, r) => (
        <div key={name} className="bk-round label" style={{ left: x(r), width: colW }}>
          {name}
        </div>
      ))}
      <div className="bk-round label" style={{ left: x(6), width: colW }}>
        Champion
      </div>
      {tour.matches.slice(0, 47).map((m, i) => {
        const r = m.round;
        const decided = m.winner !== BE_TOUR_NONE;
        const top = y(r, m.slot) - BOX_H / 2;
        return (
          <div
            key={i}
            className={`bk-match${m.live ? ' live' : ''}${decided ? ' done' : ''}`}
            style={{ left: x(r), top, width: colW, height: BOX_H }}
            title={`${ROUNDS[r]}: ${m.games} game${m.games === 1 ? '' : 's'} played${m.games > m.winsA + m.winsB ? `, ${m.games - m.winsA - m.winsB} drawn` : ''}; first to ${BE_TOUR_WIN}`}
          >
            <Side bot={m.a} seed={m.seedA} wins={m.winsA} won={decided && m.winner === m.a} lost={decided && m.winner !== m.a} bots={bots} />
            <Side bot={m.b} seed={m.seedB} wins={m.winsB} won={decided && m.winner === m.b} lost={decided && m.winner !== m.b} bots={bots} />
          </div>
        );
      })}
      <div className={`bk-champion${champ ? ' crowned' : ''}`} style={{ left: x(6), top: y(5, 0) - 44, width: colW, height: 88 }}>
        {champ ? (
          <>
            <svg width="22" height="22" viewBox="0 0 22 22" aria-hidden="true">
              <path d="M5 3 H17 V8 C17 11.5 14.5 13.5 11 13.5 C7.5 13.5 5 11.5 5 8 Z M11 13.5 V17 M7 19 H15 M5 5 H2.5 C2.5 8 3.5 9.5 5.5 9.8 M17 5 H19.5 C19.5 8 18.5 9.5 16.5 9.8" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinejoin="round" strokeLinecap="round" />
            </svg>
            <div className="bk-champ-name">
              <span className="fam-dot" style={{ background: familyColor(champ.key) }} />
              {botName(champ)}
            </div>
            <div className="faint num">
              seed {champSeed} &middot; {champ.key}
            </div>
          </>
        ) : (
          <div className="faint">{tour.state === BE_TOUR_RUNNING ? 'to be decided' : ''}</div>
        )}
      </div>
    </div>
  );
}

export function tourSummary(tour: WTour, bots: WBot[]): string {
  const decided = tour.matches.slice(0, 47).filter((m) => m.winner !== BE_TOUR_NONE).length;
  const live = tour.matches.slice(0, 47).filter((m) => m.live).length;
  const left = tour.dropped !== BE_TOUR_NONE && bots[tour.dropped] ? `${botName(bots[tour.dropped])} (${bots[tour.dropped].key}) sits out` : '';
  const head =
    tour.state === BE_TOUR_DONE
      ? `Finished: ${tour.games} rated games, ${decided} matches`
      : `${decided} of 47 matches decided, ${live} in play, ${tour.games} rated games`;
  return `48 of ${bots.length} bots, single elimination, best of five, seeded by the server's ratings. ${head}.${left ? ` ${left}.` : ''}`;
}
