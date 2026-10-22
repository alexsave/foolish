'use client';

import { ago, type StreamState, type WEvent, type WTable } from '../api';
import { BE_EV_GONE, BE_EV_HOSTILE, BE_EV_NEW, BE_EV_OVER, BE_EV_OUT, BE_EV_PICKUP, BE_EV_REFUSED, BE_EV_STALL } from '../gen/wire';
import { PlayingCard } from './PlayingCard';
import { eventText } from './TableDetail';

interface Props {
  rows: WEvent[];
  tables: Map<string, WTable>;
  onSelect: (gid: string) => void;
  paused: boolean;
  onTogglePause: () => void;
  state: StreamState;
  now: number;
}

function rowClass(e: WEvent): string {
  if (e.kind === BE_EV_HOSTILE) return e.refused ? 'feed-row hostile' : 'feed-row bad';
  if (e.kind === BE_EV_REFUSED || e.kind === BE_EV_STALL) return 'feed-row bad';
  if (e.kind === BE_EV_OVER) return 'feed-row over';
  if (e.kind === BE_EV_OUT || e.kind === BE_EV_PICKUP) return 'feed-row mark';
  if (e.kind === BE_EV_NEW || e.kind === BE_EV_GONE) return 'feed-row quiet';
  return 'feed-row';
}

export function Feed({ rows, tables, onSelect, paused, onTogglePause, state, now }: Props) {
  return (
    <section className="card feed-card">
      <div className="feed-head">
        <h2 className="card-title">Live feed</h2>
        <button type="button" className={`btn-sm${paused ? ' on' : ''}`} onClick={onTogglePause}>
          {paused ? 'Resume' : 'Pause'}
        </button>
      </div>
      <div className="feed-list">
        {rows.length === 0 ? (
          <div className="empty" style={{ padding: '12px 8px' }}>
            {state === 'open' ? 'Waiting for the first move...' : 'Connecting to birdseye...'}
          </div>
        ) : (
          rows.map((e, i) => {
            const t = tables.get(e.gid);
            const trump = t?.powerSuit ?? -1;
            return (
              <div className={rowClass(e)} key={`${e.gid}-${e.version}-${e.kind}-${Number(e.atMs)}-${i}`}>
                <button type="button" className="feed-gid num" onClick={() => onSelect(e.gid)} title="watch this table">
                  {e.gid.slice(0, 6)}
                </button>
                <span className="feed-text">{eventText(e, t?.seats)}</span>
                <span className="feed-cards">
                  {e.kind !== BE_EV_HOSTILE && e.cards.slice(0, 6).map((c, k) => <PlayingCard key={k} card={c} size="xs" trump={c.suit === trump} />)}
                </span>
                <span className="feed-ago num">{ago(Number(e.atMs), now)}</span>
              </div>
            );
          })
        )}
      </div>
    </section>
  );
}
