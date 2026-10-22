'use client';

import { useEffect, useMemo, useRef, useState } from 'react';
import { EVERYONE, USER_KIND, type FeedState, type WUser } from '../api';

// The composite of every user, as a row of the picker (api.ts EVERYONE).
const EVERYONE_ROW: WUser = { id: EVERYONE, kind: 0, pad: 0, nActivities: 0, version: 0, uniqueM: [0, 0, 0], homeLat: 0, homeLon: 0, name: 'Everyone' };

function Compass() {
  return (
    <svg width="26" height="26" viewBox="0 0 26 26" fill="none" aria-hidden="true">
      <circle cx="13" cy="13" r="11.5" stroke="#fff" strokeWidth="1.5" />
      <path d="M13 4.5 L15.4 13 L13 21.5 L10.6 13 Z" fill="#fff" />
      <path d="M13 4.5 L15.4 13 L13 13 Z" fill="#e0453a" />
      <path d="M13 4.5 L10.6 13 L13 13 Z" fill="#e0453a" />
      <circle cx="13" cy="13" r="1.4" fill="#1b2a41" />
    </svg>
  );
}

interface Props {
  users: WUser[];
  selected: number | null;
  onSelect: (id: number) => void;
  feedState: FeedState;
  connecting: boolean;
}

export function TopBar({ users, selected, onSelect, feedState, connecting }: Props) {
  const [query, setQuery] = useState('');
  const [open, setOpen] = useState(false);
  const [active, setActive] = useState(0);
  const root = useRef<HTMLDivElement>(null);
  const current = selected === EVERYONE ? EVERYONE_ROW : selected !== null ? users.find((u) => u.id === selected) ?? null : null;

  const sorted = useMemo(
    () => [EVERYONE_ROW, ...users.slice().sort((a, b) => b.nActivities - a.nActivities || a.name.localeCompare(b.name))],
    [users],
  );
  const matches = useMemo(() => {
    const q = query.trim().toLowerCase();
    const list = q
      ? sorted.filter((u) => u.name.toLowerCase().includes(q) || (USER_KIND[u.kind] ?? '').includes(q))
      : sorted;
    return list.slice(0, 60);
  }, [sorted, query]);

  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      if (root.current && !root.current.contains(e.target as Node)) setOpen(false);
    };
    window.addEventListener('mousedown', onDown);
    return () => window.removeEventListener('mousedown', onDown);
  }, [open]);

  const pick = (u: WUser) => {
    onSelect(u.id);
    setQuery('');
    setOpen(false);
  };

  const onKey = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      setOpen(true);
      setActive((a) => Math.min(a + 1, matches.length - 1));
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      setActive((a) => Math.max(a - 1, 0));
    } else if (e.key === 'Enter') {
      const u = matches[active];
      if (u) pick(u);
    } else if (e.key === 'Escape') {
      setOpen(false);
      setQuery('');
      (e.target as HTMLInputElement).blur();
    }
  };

  const status =
    connecting ? { cls: 'dot-amber', text: 'connecting...' }
    : feedState === 'open' ? { cls: 'dot-green', text: 'live' }
    : feedState === 'connecting' ? { cls: 'dot-amber', text: 'feed connecting...' }
    : { cls: 'dot-red', text: 'feed offline' };

  return (
    <header className="topbar">
      <div className="brand">
        <Compass />
        <div>
          <div className="wordmark">Adventurer</div>
          <div className="tagline">unique roads, counted in C</div>
        </div>
      </div>
      <div className="topbar-right">
        <span className="conn">
          <span className={`dot ${status.cls}`} />
          <span className="conn-text">{status.text}</span>
        </span>
        <div className="picker" ref={root}>
          <input
            type="text"
            value={open ? query : current ? current.name : query}
            placeholder={users.length ? 'Find a user' : 'Loading users...'}
            disabled={!users.length}
            onFocus={() => {
              setOpen(true);
              setActive(0);
            }}
            onChange={(e) => {
              setQuery(e.target.value);
              setActive(0);
              setOpen(true);
            }}
            onKeyDown={onKey}
            aria-label="Select user"
          />
          <svg className="picker-caret" width="12" height="12" viewBox="0 0 12 12" aria-hidden="true">
            <path d="M2 4.5 L6 8.5 L10 4.5" stroke="currentColor" strokeWidth="1.5" fill="none" />
          </svg>
          {open && matches.length > 0 && (
            <div className="picker-list" role="listbox">
              {matches.map((u, i) => (
                <button
                  key={u.id}
                  type="button"
                  role="option"
                  aria-selected={u.id === selected}
                  className={`picker-item${i === active ? ' active' : ''}`}
                  onMouseDown={(e) => {
                    e.preventDefault();
                    pick(u);
                  }}
                  onMouseEnter={() => setActive(i)}
                >
                  <span className="name">{u.name}</span>
                  <span className="pill pill-grey">{u.id === EVERYONE ? 'all users' : USER_KIND[u.kind] ?? 'user'}</span>
                  <span className="count num">{u.id === EVERYONE ? '' : u.nActivities}</span>
                </button>
              ))}
            </div>
          )}
        </div>
      </div>
    </header>
  );
}
