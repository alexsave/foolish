'use client';

// The Bot Cup (wire.h be_w_cup, botcup.h): the four contest bots' round robin of best-of-five
// matches, one at a time, then the final between the top two; the standings birdseye works out,
// every match game by game, and the table in play. Everything is birdseye's record; this file only
// draws it.

import type { WBot, WCup } from '../api';
import {
  BE_CUP_G_A,
  BE_CUP_G_B,
  BE_CUP_G_DRAW,
  BE_CUP_ROBIN,
  BE_TOUR_DONE,
  BE_TOUR_MAX_GAMES_W,
  BE_TOUR_NONE,
  BE_TOUR_WIN,
} from '../gen/wire';
import { botName, familyColor } from './League';

interface Props {
  cup: WCup;
  bots: WBot[];
}

// The game the page shows beside the scoreboard: the one in play, else the current match's last.
export function cupTableGid(cup: WCup): string | null {
  const live = cup.matches.find((m) => m.live && m.gid);
  if (live) return live.gid;
  const cur = cup.matches.find((m) => m.winner === BE_TOUR_NONE && m.a !== BE_TOUR_NONE && m.gid);
  if (cur) return cur.gid;
  const last = [...cup.matches].reverse().find((m) => m.gid);
  return last ? last.gid : null;
}

function Who({ b }: { b: WBot | undefined }) {
  if (!b) return <span className="faint">to be decided</span>;
  return (
    <span className="ga-who">
      <span className="fam-dot" style={{ background: familyColor(b.key) }} />
      <span className="ga-name">{botName(b)}</span>
    </span>
  );
}

export function BotCup({ cup, bots }: Props) {
  const played = (n: number) => `${n} ${n === 1 ? 'game' : 'games'} played.`;
  const name = (i: number) => (i !== BE_TOUR_NONE && bots[i] ? botName(bots[i]) : 'tbd');
  const current = cup.matches.findIndex((m) => m.winner === BE_TOUR_NONE && m.a !== BE_TOUR_NONE);
  const done = cup.state === BE_TOUR_DONE;
  const sub = done
    ? `${name(cup.champion)} wins the cup after ${cup.games} games.`
    : current >= 0
      ? `${current < BE_CUP_ROBIN ? `Match ${current + 1} of ${BE_CUP_ROBIN}` : 'The final'}: ${name(cup.matches[current].a)} v ${name(cup.matches[current].b)}. ${played(cup.games)}`
      : played(cup.games);
  return (
    <section className="card tour-card ga-card">
      <div className="chart-head">
        <div>
          <h2 className="card-title">Bot Cup</h2>
          <div className="chart-sub">
            Four contest bots, every pair once in a two-player best of five (first to {BE_TOUR_WIN}), one match at a time, then a final of
            the top two. {sub}
          </div>
        </div>
      </div>
      <h3 className="cup-h">Standings</h3>
      <table className="cup-table num">
        <thead>
          <tr>
            <th />
            <th className="l">Bot</th>
            <th title="matches won-lost">Matches</th>
            <th title="games won-lost">Games</th>
            <th title="games won less games lost">Diff</th>
          </tr>
        </thead>
        <tbody>
          {cup.order.map((k, r) => {
            const b = bots[cup.bots[k]];
            const d = cup.gw[k] - cup.gl[k];
            return (
              <tr key={k} className={done && cup.bots[k] === cup.champion ? 'champ' : ''}>
                <td className="faint">{r + 1}</td>
                <td className="l">
                  <Who b={b} />
                </td>
                <td>
                  {cup.mw[k]}-{cup.ml[k]}
                </td>
                <td>
                  {cup.gw[k]}-{cup.gl[k]}
                </td>
                <td>{d > 0 ? `+${d}` : d}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
      <h3 className="cup-h">Matches</h3>
      <ol className="ga-ladder">
        {cup.matches.map((m, i) => {
          const final = i >= BE_CUP_ROBIN;
          const now_ = i === current && !done;
          const decided = m.winner !== BE_TOUR_NONE;
          const games = cup.result.slice(i * BE_TOUR_MAX_GAMES_W, i * BE_TOUR_MAX_GAMES_W + Math.min(m.games, BE_TOUR_MAX_GAMES_W));
          return (
            <li
              key={i}
              className={`ga-rung cup-match${now_ ? ' current' : ''}${decided ? '' : now_ ? '' : ' ahead'}${final ? ' final' : ''}`}
            >
              <span className="ga-no num">{final ? 'F' : i + 1}</span>
              <span className="cup-pair">
                <span className={m.winner === m.a && decided ? 'cup-win' : ''}>
                  <Who b={m.a === BE_TOUR_NONE ? undefined : bots[m.a]} />
                </span>
                <span className="faint">v</span>
                <span className={m.winner === m.b && decided ? 'cup-win' : ''}>
                  <Who b={m.b === BE_TOUR_NONE ? undefined : bots[m.b]} />
                </span>
              </span>
              <span className="ga-games" aria-label="games">
                {games.map((x, g) => {
                  const by = x === BE_CUP_G_A ? m.a : x === BE_CUP_G_B ? m.b : BE_TOUR_NONE;
                  const label = x === BE_CUP_G_DRAW ? 'D' : name(by).slice(0, 1).toUpperCase();
                  const bg = by !== BE_TOUR_NONE && bots[by] ? familyColor(bots[by].key) : 'var(--faint)';
                  return (
                    <span
                      key={g}
                      className="ga-game"
                      style={{ background: bg }}
                      title={`game ${g + 1}: ${x === BE_CUP_G_DRAW ? 'drawn (played again)' : `${name(by)} won`}`}
                    >
                      {label}
                    </span>
                  );
                })}
              </span>
              <span className="ga-score num">{m.games ? `${m.winsA}-${m.winsB}` : ''}</span>
              <span className="ga-status">
                {now_ ? (
                  <span className="ga-tag now">{m.live ? 'live' : 'next'}</span>
                ) : decided ? (
                  <span className="ga-tag won">{final ? 'champion' : 'done'}</span>
                ) : null}
              </span>
            </li>
          );
        })}
      </ol>
    </section>
  );
}
