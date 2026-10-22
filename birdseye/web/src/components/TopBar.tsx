'use client';

import { gauge, metric, perS, type Metrics, type StreamState } from '../api';

function Eye() {
  return (
    <svg width="28" height="28" viewBox="0 0 28 28" fill="none" aria-hidden="true">
      <path d="M2.5 14 C7 6.5 21 6.5 25.5 14 C21 21.5 7 21.5 2.5 14 Z" stroke="#fff" strokeWidth="1.6" strokeLinejoin="round" />
      <circle cx="14" cy="14" r="4.6" fill="#fff" />
      <circle cx="14" cy="14" r="2.2" fill="#1b2a41" />
      <circle cx="15.6" cy="12.4" r="0.9" fill="#fff" />
    </svg>
  );
}

interface Props {
  state: StreamState;
  ops: Metrics | null;
  filter: Filter;
  onFilter: (f: Filter) => void;
  counts: { all: number; playing: number; lobby: number; over: number };
  compact: boolean;
  onCompact: (c: boolean) => void;
  // The league's view switch; null when birdseye runs the mixed sim (no league to show).
  view: View | null;
  onView: (v: View) => void;
  games: number;
  gamesPerMin: number | null;
}

export type Filter = 'all' | 'playing' | 'lobby' | 'over';
export type View = 'league' | 'tables';

export function TopBar({ state, ops, filter, onFilter, counts, compact, onCompact, view, onView, games, gamesPerMin }: Props) {
  const status =
    state === 'open' ? { cls: 'dot-green', text: 'live' }
    : state === 'connecting' ? { cls: 'dot-amber', text: 'connecting...' }
    : { cls: 'dot-grey', text: 'reconnecting...' };
  const tabs: { key: Filter; label: string; n: number }[] = [
    { key: 'all', label: 'All', n: counts.all },
    { key: 'playing', label: 'Playing', n: counts.playing },
    { key: 'lobby', label: 'Lobby', n: counts.lobby },
    { key: 'over', label: 'Finished', n: counts.over },
  ];
  return (
    <header className="topbar">
      <div className="brand">
        <Eye />
        <div>
          <div className="wordmark">Birdseye</div>
          <div className="tagline">every foolish table, live</div>
        </div>
      </div>
      {view !== null && (
        <nav className="tabs" aria-label="view">
          <button type="button" className={`tab${view === 'league' ? ' on' : ''}`} onClick={() => onView('league')}>
            League <span className="num">{games}</span>
          </button>
          <button type="button" className={`tab${view === 'tables' ? ' on' : ''}`} onClick={() => onView('tables')}>
            Tables <span className="num">{counts.all}</span>
          </button>
        </nav>
      )}
      {view !== 'league' && (
        <nav className="tabs" aria-label="table filter">
          {tabs.map((t) => (
            <button key={t.key} type="button" className={`tab${filter === t.key ? ' on' : ''}`} onClick={() => onFilter(t.key)}>
              {t.label} <span className="num">{t.n}</span>
            </button>
          ))}
        </nav>
      )}
      {view !== 'league' && (
      <button type="button" className={`density${compact ? ' on' : ''}`} onClick={() => onCompact(!compact)} title="smaller cards: more tables on one screen">
        <svg width="14" height="14" viewBox="0 0 14 14" aria-hidden="true">
          <rect x="1" y="1" width="5" height="5" rx="1" fill="currentColor" />
          <rect x="8" y="1" width="5" height="5" rx="1" fill="currentColor" />
          <rect x="1" y="8" width="5" height="5" rx="1" fill="currentColor" />
          <rect x="8" y="8" width="5" height="5" rx="1" fill="currentColor" />
        </svg>
        Compact
      </button>
      )}
      <div className="topbar-right">
        {ops && (
          <span className="topbar-stat num">
            {view !== null ? (
              <>
                <b>{perS(ops, 'moves').toFixed(1)}</b> moves/s &middot; <b>{(gamesPerMin ?? perS(ops, 'games') * 60).toFixed(1)}</b> games/min &middot;{' '}
                <b>{gauge(ops, 'bots_seated')}</b> bots seated &middot; server CPU <b>{(gauge(ops, 'server_cpu_cpct') / 100).toFixed(1)}</b>%
              </>
            ) : (
              <>
                <b>{perS(ops, 'moves').toFixed(1)}</b> moves/s &middot; <b>{gauge(ops, 'players_seated')}</b> seated &middot; ack p99{' '}
                <b>{((metric(ops, 'move_ack_us')?.p99 ?? 0) / 1000).toFixed(1)}</b> ms
              </>
            )}
          </span>
        )}
        <span className="conn">
          <span className={`dot ${status.cls}`} />
          <span className="conn-text">{status.text}</span>
        </span>
      </div>
    </header>
  );
}
