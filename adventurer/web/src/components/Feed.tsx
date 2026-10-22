'use client';

import { useEffect, useState } from 'react';
import {
  AF_BACKFILL,
  AF_REJECTED,
  AF_TRUTH,
  ago,
  km,
  msFromUs,
  REJECT_REASON,
  type FeedState,
  type WActivity,
  type WUser,
} from '../api';

interface Props {
  rows: WActivity[];
  usersById: Map<number, WUser>;
  onSelectUser: (id: number) => void;
  rate: number;
  paused: boolean;
  onTogglePause: () => void;
  feedState: FeedState;
}

// ADV_ACT_BIKE 1, RUN 2, WALK 3.
function Glyph({ type }: { type: number }) {
  if (type === 1) {
    return (
      <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4" aria-label="bike">
        <circle cx="4" cy="11" r="2.6" />
        <circle cx="12" cy="11" r="2.6" />
        <path d="M4 11 L7 5 L10 5 M7 5 L9.5 11 M9.5 11 L12 11 M10 5 L12 11" strokeLinejoin="round" />
      </svg>
    );
  }
  if (type === 2) {
    return (
      <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4" aria-label="run">
        <circle cx="10.5" cy="3" r="1.5" />
        <path d="M9 6 L6 8 L4 11 M9 6 L10 9.5 L12.5 11 M9 6 L12 5 M6 8 L8.5 10 L7 14" strokeLinecap="round" strokeLinejoin="round" />
      </svg>
    );
  }
  return (
    <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4" aria-label="walk">
      <circle cx="8.5" cy="3" r="1.5" />
      <path d="M8 6 L6.5 10 L5.5 14 M8 6 L9.5 9.5 L9 14 M8 6 L10.5 8" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}

export function Feed({ rows, usersById, onSelectUser, rate, paused, onTogglePause, feedState }: Props) {
  // The "ago" column ticks without a new frame.
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 10000);
    return () => clearInterval(t);
  }, []);
  const seen = rows.length ? Math.max(now, rows[0].atMs) : now;

  return (
    <section className="card feed-card">
      <div className="feed-head">
        <h2 className="card-title">Live feed</h2>
        <span className="feed-rate num">
          <b>{rate.toFixed(1)}</b> uploads/s
        </span>
        <button type="button" className={`btn-sm${paused ? ' on' : ''}`} onClick={onTogglePause}>
          {paused ? 'Resume' : 'Pause'}
        </button>
      </div>
      <div className="feed-list">
        {rows.length === 0 ? (
          <div className="empty" style={{ padding: '12px 8px' }}>
            {feedState === 'open' ? 'Waiting for uploads...' : 'Connecting to the feed...'}
          </div>
        ) : (
          rows.map((a) => {
            const rejected = (a.flags & AF_REJECTED) !== 0;
            const name = usersById.get(a.user)?.name ?? `user ${a.user}`;
            const newM = a.newM[2];
            return (
              <div className={`feed-row${rejected ? ' rejected' : ''}`} key={a.activity || `${a.user}-${a.atMs}-${a.genId}`}>
                <span className="glyph">
                  <Glyph type={a.type} />
                </span>
                <div className="feed-main">
                  <div className="feed-line1">
                    <button type="button" className="feed-user" onClick={() => onSelectUser(a.user)}>
                      {name}
                    </button>
                    {!rejected && (
                      <span className={`feed-new num${newM > 0 ? ' pos' : ''}`}>+{km(newM)} km new</span>
                    )}
                    {rejected && <span className="pill pill-red">rejected: {REJECT_REASON[a.reason] ?? a.reason}</span>}
                    {(a.flags & AF_BACKFILL) !== 0 && <span className="pill pill-grey">backfill</span>}
                  </div>
                  <div className="feed-line2 num">
                    <span>{km(a.matchedM)} km matched</span>
                    {(a.flags & AF_TRUTH) !== 0 && (
                      <span className={a.accuracy < 0.9 ? 'bad' : ''}>acc {Math.round(a.accuracy * 100)}%</span>
                    )}
                    <span>cpu {msFromUs(a.cpuUs)} ms</span>
                    <span>e2e {msFromUs(a.e2eUs)} ms</span>
                  </div>
                </div>
                <span className="feed-ago num">{ago(a.atMs, seen)}</span>
              </div>
            );
          })
        )}
      </div>
    </section>
  );
}
