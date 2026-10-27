'use client';

// The gauntlet's ladder (wire.h be_w_gauntlet, gauntlet.h): seven rungs, rung 1 at the foot, each
// the bot of one brain the human meets in a best of five; the games' results, the score, the rung he
// is on, and a link to the game in play. Everything is birdseye's record; this file only draws it.

import { PLAY_URL, type WBot, type WGauntlet } from '../api';
import { BE_GAUNT_G_BOT, BE_GAUNT_G_DRAW, BE_GAUNT_G_HUMAN, BE_GAUNT_LOST, BE_GAUNT_RUNNING, BE_GAUNT_WON, BE_TOUR_NONE, BE_TOUR_WIN } from '../gen/wire';
import { botName, familyColor } from './League';

interface Props {
  g: WGauntlet;
  bots: WBot[];
}

const MARK: Record<number, { text: string; cls: string; title: string }> = {
  [BE_GAUNT_G_HUMAN]: { text: 'W', cls: 'won', title: 'won' },
  [BE_GAUNT_G_BOT]: { text: 'L', cls: 'lost', title: 'lost' },
  [BE_GAUNT_G_DRAW]: { text: 'D', cls: 'drawn', title: 'drawn (played again)' },
};

export function gauntletSummary(g: WGauntlet, bots: WBot[]): string {
  const human = g.human !== BE_TOUR_NONE && bots[g.human] ? botName(bots[g.human]) : 'The human';
  const where =
    g.state === BE_GAUNT_WON
      ? `climbed all ${g.rungs.length} rungs`
      : g.state === BE_GAUNT_LOST
        ? `fell at rung ${g.rung + 1}`
        : `on rung ${g.rung + 1} of ${g.rungs.length}`;
  const best = g.bestRung ? ` Best run: rung ${g.bestRung} reached, ${g.bestWon} won (run ${g.bestRun}).` : '';
  return `${human} against one bot of each brain, weakest first, best of five a rung. Run ${g.run}: ${where}, ${g.games} games played.${best}`;
}

export function Gauntlet({ g, bots }: Props) {
  const rows = g.rungs.map((r, i) => ({ r, i })).reverse(); // the top rung first, rung 1 at the foot
  return (
    <section className="card tour-card ga-card">
      <div className="chart-head">
        <div>
          <h2 className="card-title">Gauntlet</h2>
          <div className="chart-sub">{gauntletSummary(g, bots)}</div>
        </div>
      </div>
      <ol className="ga-ladder">
        {rows.map(({ r, i }) => {
          const b = r.bot === BE_TOUR_NONE ? null : bots[r.bot];
          const current = g.state === BE_GAUNT_RUNNING && i === g.rung;
          const won = i < g.rung || g.state === BE_GAUNT_WON;
          const lost = g.state === BE_GAUNT_LOST && i === g.rung;
          const ahead = !won && !lost && !current;
          const status = won ? 'won' : lost ? 'lost' : current ? (r.live ? 'playing' : 'next') : '';
          return (
            <li key={i} className={`ga-rung${current ? ' current' : ''}${won ? ' won' : ''}${lost ? ' lost' : ''}${ahead ? ' ahead' : ''}`}>
              <span className="ga-no num">{i + 1}</span>
              <span className="ga-who">
                {b ? <span className="fam-dot" style={{ background: familyColor(b.key) }} /> : <span className="fam-dot empty" />}
                <span className="ga-name">{b ? botName(b) : 'to be drawn'}</span>
                <span className="ga-brain faint">{b?.key ?? ''}</span>
              </span>
              <span className="ga-rating num" title="the bot's server rating when this run's ladder was drawn">
                {r.rating}
              </span>
              <span className="ga-games" aria-label="games">
                {r.result.slice(0, Math.min(r.games, r.result.length)).map((x, k) => {
                  const m = MARK[x];
                  return m ? (
                    <span key={k} className={`ga-game ${m.cls}`} title={`game ${k + 1}: ${m.title}`}>
                      {m.text}
                    </span>
                  ) : null;
                })}
              </span>
              <span className="ga-score num" title={`first to ${BE_TOUR_WIN}`}>
                {r.games ? `${r.winsH}-${r.winsB}` : ''}
              </span>
              <span className="ga-status">
                {status && <span className={`ga-tag ${status}`}>{status}</span>}
                {current && r.live && r.gid && PLAY_URL ? (
                  <a className="bk-play" href={`${PLAY_URL}/${r.gid}`} target="_blank" rel="noreferrer" title="play this game in the foolish web UI">
                    play
                  </a>
                ) : null}
              </span>
            </li>
          );
        })}
      </ol>
    </section>
  );
}
