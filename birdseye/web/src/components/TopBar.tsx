'use client';

import { type StreamState, type WOps } from '../api';

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
  ops: WOps | null;
  filter: Filter;
  onFilter: (f: Filter) => void;
  counts: { all: number; playing: number; lobby: number; over: number };
}

export type Filter = 'all' | 'playing' | 'lobby' | 'over';

export function TopBar({ state, ops, filter, onFilter, counts }: Props) {
  const status =
    state === 'open' ? { cls: 'dot-green', text: 'live' }
    : state === 'connecting' ? { cls: 'dot-amber', text: 'connecting...' }
    : { cls: 'dot-red', text: 'offline' };
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
      <nav className="tabs" aria-label="table filter">
        {tabs.map((t) => (
          <button key={t.key} type="button" className={`tab${filter === t.key ? ' on' : ''}`} onClick={() => onFilter(t.key)}>
            {t.label} <span className="num">{t.n}</span>
          </button>
        ))}
      </nav>
      <div className="topbar-right">
        {ops && (
          <span className="topbar-stat num">
            <b>{ops.movesPerS.toFixed(1)}</b> moves/s &middot; <b>{ops.playersSeated}</b> seated &middot; ack p99 <b>{(ops.actP99Us / 1000).toFixed(1)}</b> ms
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
