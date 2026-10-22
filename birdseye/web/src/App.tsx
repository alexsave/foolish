'use client';

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { getMetrics, getTables, openStream, perS, WireMismatch, type Metrics, type StreamState, type WEvent, type WTable } from './api';
import { BE_PHASE_GONE, BE_PHASE_LOBBY, BE_PHASE_OVER, BE_PHASE_PLAYING } from './gen/wire';
import { Feed } from './components/Feed';
import { Ops } from './components/Ops';
import { TableCard } from './components/TableCard';
import { TableDetail } from './components/TableDetail';
import { TopBar, type Filter } from './components/TopBar';

const FEED_CAP = 200;
const TABLE_EVENTS_CAP = 64;
const SPARK_N = 60;
const LIST_RETRY_MS = 2000;

export function App() {
  const [fatal, setFatal] = useState<string | null>(null);
  const [state, setState] = useState<StreamState>('connecting');
  const [tables, setTables] = useState<Map<string, WTable>>(() => new Map());
  const [versions, setVersions] = useState<Map<string, number>>(() => new Map());
  const [events, setEvents] = useState<WEvent[]>([]);
  const [byTable, setByTable] = useState<Map<string, WEvent[]>>(() => new Map());
  const [ops, setOps] = useState<Metrics | null>(null);
  const [spark, setSpark] = useState<number[]>([]);
  const [paused, setPaused] = useState(false);
  const pausedRef = useRef(false);
  pausedRef.current = paused;
  const pending = useRef<WEvent[]>([]);
  const [selected, setSelected] = useState<string | null>(null);
  const [filter, setFilter] = useState<Filter>('all');
  const [now, setNow] = useState(() => Date.now());

  // The "ago" columns tick without a new frame.
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);

  // The first picture: every table the sim holds, and the numbers, before the stream catches up.
  useEffect(() => {
    let live = true;
    let timer: ReturnType<typeof setTimeout> | null = null;
    const load = async () => {
      try {
        const [list, o] = await Promise.all([getTables(), getMetrics()]);
        if (!live) return;
        setTables((prev) => {
          const next = new Map(prev);
          for (const t of list) {
            const have = next.get(t.gid);
            if (!have || have.version <= t.version) next.set(t.gid, t);
          }
          return next;
        });
        setOps((prev) => prev ?? o);
      } catch (e) {
        if (!live) return;
        if (e instanceof WireMismatch) {
          setFatal(e.message);
          return;
        }
        timer = setTimeout(load, LIST_RETRY_MS);
      }
    };
    load();
    return () => {
      live = false;
      if (timer) clearTimeout(timer);
    };
  }, []);

  useEffect(() => {
    const close = openStream({
      onState: setState,
      onError: (e) => {
        if (e instanceof WireMismatch) setFatal(e.message);
      },
      onOps: (o) => {
        setOps(o);
        setSpark((s) => {
          const next = s.length >= SPARK_N ? s.slice(s.length - SPARK_N + 1) : s.slice();
          next.push(perS(o, 'moves'));
          return next;
        });
      },
      onTable: (t) => {
        setTables((prev) => {
          const next = new Map(prev);
          next.set(t.gid, t);
          return next;
        });
        setVersions((prev) => {
          const next = new Map(prev);
          next.set(t.gid, (next.get(t.gid) ?? 0) + 1);
          return next;
        });
      },
      onEvent: (e) => {
        setByTable((prev) => {
          const next = new Map(prev);
          const rows = [e, ...(next.get(e.gid) ?? [])];
          next.set(e.gid, rows.length > TABLE_EVENTS_CAP ? rows.slice(0, TABLE_EVENTS_CAP) : rows);
          return next;
        });
        if (pausedRef.current) {
          pending.current.push(e);
          if (pending.current.length > FEED_CAP) pending.current.splice(0, pending.current.length - FEED_CAP);
        } else {
          setEvents((rows) => {
            const next = [e, ...rows];
            return next.length > FEED_CAP ? next.slice(0, FEED_CAP) : next;
          });
        }
      },
    });
    return close;
  }, []);

  // Tables the sim forgot (GONE for a while) leave the page too: once a minute, by the row's own
  // changed time. The sim keeps them 90 s; the page keeps them 2 minutes.
  useEffect(() => {
    const t = setInterval(() => {
      setTables((prev) => {
        let changed = false;
        const next = new Map(prev);
        for (const [gid, row] of prev) {
          if (row.phase === BE_PHASE_GONE && Date.now() - Number(row.changedMs) > 120000) {
            next.delete(gid);
            changed = true;
          }
        }
        return changed ? next : prev;
      });
    }, 60000);
    return () => clearInterval(t);
  }, []);

  const togglePause = useCallback(() => {
    setPaused((p) => {
      if (p && pending.current.length) {
        const buffered = pending.current.reverse();
        pending.current = [];
        setEvents((rows) => [...buffered, ...rows].slice(0, FEED_CAP));
      }
      return !p;
    });
  }, []);

  // The grid: playing first (most recently changed first), then lobbies, then finished, then gone.
  const rows = useMemo(() => {
    const all = Array.from(tables.values());
    const rank = (t: WTable) => (t.phase === BE_PHASE_PLAYING ? 0 : t.phase === BE_PHASE_LOBBY ? 1 : t.phase === BE_PHASE_OVER ? 2 : 3);
    all.sort((a, b) => rank(a) - rank(b) || Number(b.createdMs - a.createdMs));
    return all;
  }, [tables]);
  const counts = useMemo(() => {
    const c = { all: 0, playing: 0, lobby: 0, over: 0 };
    for (const t of rows) {
      if (t.phase === BE_PHASE_GONE) continue;
      c.all++;
      if (t.phase === BE_PHASE_PLAYING) c.playing++;
      else if (t.phase === BE_PHASE_LOBBY) c.lobby++;
      else if (t.phase === BE_PHASE_OVER) c.over++;
    }
    return c;
  }, [rows]);
  const shown = useMemo(
    () =>
      rows.filter((t) =>
        filter === 'all' ? t.phase !== BE_PHASE_GONE
        : filter === 'playing' ? t.phase === BE_PHASE_PLAYING
        : filter === 'lobby' ? t.phase === BE_PHASE_LOBBY
        : t.phase === BE_PHASE_OVER || t.phase === BE_PHASE_GONE,
      ),
    [rows, filter],
  );

  // The selected table is the URL's hash, so a table can be linked to and survives a reload.
  useEffect(() => {
    const read = () => {
      const h = window.location.hash.replace(/^#/, '');
      setSelected(h ? decodeURIComponent(h) : null);
    };
    read();
    window.addEventListener('hashchange', read);
    return () => window.removeEventListener('hashchange', read);
  }, []);
  const select = useCallback((gid: string) => {
    window.location.hash = encodeURIComponent(gid);
    setSelected(gid);
  }, []);
  const back = useCallback(() => {
    window.history.replaceState(null, '', window.location.pathname);
    setSelected(null);
  }, []);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') back();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [back]);

  return (
    <div className="app">
      <TopBar state={state} ops={ops} filter={filter} onFilter={setFilter} counts={counts} />
      {fatal ? (
        <div className="banner">{fatal}</div>
      ) : (
        <div className="grid">
          <div className="col col-main">
            {selected ? (
              <TableDetail
                gid={selected}
                live={tables.get(selected) ?? null}
                version={versions.get(selected) ?? 0}
                events={byTable.get(selected) ?? []}
                now={now}
                onBack={back}
              />
            ) : shown.length === 0 ? (
              <div className="card empty-grid">
                {state === 'open' ? (tables.size === 0 ? 'No tables yet: the sim is signing its players up.' : 'No tables in this view.') : 'Connecting to birdseye...'}
              </div>
            ) : (
              <div className="tgrid">
                {shown.map((t) => (
                  <TableCard key={t.gid} table={t} now={now} selected={false} onSelect={select} />
                ))}
              </div>
            )}
          </div>
          <div className="col col-side">
            <Ops ops={ops} spark={spark} />
            <Feed rows={events} tables={tables} onSelect={select} paused={paused} onTogglePause={togglePause} state={state} now={now} />
          </div>
        </div>
      )}
    </div>
  );
}
