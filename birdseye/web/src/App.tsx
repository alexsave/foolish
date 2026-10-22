'use client';

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  getLeague,
  getMetrics,
  getTables,
  openStream,
  perS,
  WireMismatch,
  type League,
  type Metrics,
  type StreamState,
  type WEvent,
  type WResult,
  type WTable,
} from './api';
import { BE_MODE_LEAGUE, BE_PHASE_GONE, BE_PHASE_LOBBY, BE_PHASE_OVER, BE_PHASE_PLAYING } from './gen/wire';
import { Feed } from './components/Feed';
import { buildSeries, EloChart, Families, Standings, botName } from './components/League';
import { Ops } from './components/Ops';
import { TableDetail } from './components/TableDetail';
import { TableGrid } from './components/TableGrid';
import { TopBar, type Filter, type View } from './components/TopBar';

const FEED_CAP = 200;
const TABLE_EVENTS_CAP = 64;
const SPARK_N = 60;
const LIST_RETRY_MS = 2000;

// The results after the last one `l` holds, appended in order (a stream result already in the
// snapshot, or one older, is dropped).
function mergeResults(l: League | null, add: WResult[]): League | null {
  if (!l) return l;
  let last = l.results.length ? l.results[l.results.length - 1].seq : 0;
  const more: WResult[] = [];
  for (const r of [...add].sort((a, b) => a.seq - b.seq)) {
    if (r.seq <= last) continue;
    more.push(r);
    last = r.seq;
  }
  return more.length ? { ...l, results: [...l.results, ...more] } : l;
}

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
  // Compact cards (more tables on one screen): a per-viewer choice, kept in the browser.
  const [compact, setCompactState] = useState(false);
  useEffect(() => {
    // ?compact=1 in the URL wins (a link, a screenshot); else the viewer's last choice.
    const q = new URLSearchParams(window.location.search).get('compact');
    if (q !== null) {
      setCompactState(q === '1');
      return;
    }
    try {
      setCompactState(window.localStorage.getItem('birdseye.compact') === '1');
    } catch {
      // storage blocked: the default stands
    }
  }, []);
  const setCompact = useCallback((c: boolean) => {
    setCompactState(c);
    try {
      window.localStorage.setItem('birdseye.compact', c ? '1' : '0');
    } catch {
      // the choice just does not survive a reload
    }
  }, []);
  const [now, setNow] = useState(() => Date.now());

  // ---- the league: the standings and every rated game, from GET /be/league and then the stream.
  const [league, setLeague] = useState<League | null>(null);
  const [flash, setFlash] = useState<Map<number, number>>(() => new Map());
  const [focus, setFocus] = useState<number | null>(null);
  const [hoverBot, setHoverBot] = useState<number | null>(null);
  const [hidden, setHidden] = useState<Set<string>>(() => new Set());
  const [view, setViewState] = useState<View>('league');
  // The results the stream carried lately: a snapshot that was answered before they landed is
  // completed with them, so no result falls between the read and the stream.
  const recent = useRef<WResult[]>([]);
  const loadLeague = useCallback(async () => {
    try {
      const l = await getLeague();
      setLeague(mergeResults(l, recent.current));
    } catch (e) {
      if (e instanceof WireMismatch) setFatal(e.message);
    }
  }, []);
  useEffect(() => {
    const q = new URLSearchParams(window.location.search).get('view');
    if (q === 'tables' || q === 'league') setViewState(q);
  }, []);
  const setView = useCallback((v: View) => {
    setViewState(v);
    const u = new URL(window.location.href);
    u.searchParams.set('view', v);
    window.history.replaceState(null, '', u.toString());
  }, []);

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

  // The league read again each time the stream opens: a result missed while it was down is in it.
  useEffect(() => {
    if (state === 'open') loadLeague();
  }, [state, loadLeague]);

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
      onResult: (r) => {
        recent.current.push(r);
        if (recent.current.length > 256) recent.current.splice(0, recent.current.length - 256);
        setLeague((l) => mergeResults(l, [r]));
        setFlash((f) => {
          const next = new Map(f);
          const t = performance.now();
          for (const b of r.bot) next.set(b, t);
          return next;
        });
      },
      onBot: (b) => {
        setLeague((l) => {
          if (!l) return l;
          const i = l.bots.findIndex((x) => x.id === b.id);
          if (i < 0) return l;
          const bots = l.bots.slice();
          bots[i] = b;
          return { ...l, bots };
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

  const isLeague = league?.mode === BE_MODE_LEAGUE;
  const showLeague = isLeague && view === 'league' && !selected;
  const series = useMemo(() => (league ? buildSeries(league.bots, league.results) : []), [league]);
  const lastResult = league && league.results.length ? league.results[league.results.length - 1] : null;
  const liveTables = useMemo(() => rows.filter((t) => t.phase !== BE_PHASE_GONE), [rows]);
  const toggleFamily = useCallback((k: string) => {
    setHidden((h) => {
      const next = new Set(h);
      if (next.has(k)) next.delete(k);
      else next.add(k);
      return next;
    });
  }, []);

  return (
    <div className="app">
      <TopBar
        state={state}
        ops={ops}
        filter={filter}
        onFilter={setFilter}
        counts={counts}
        compact={compact}
        onCompact={setCompact}
        view={isLeague ? (selected ? 'tables' : view) : null}
        onView={(v) => {
          if (selected) back();
          setView(v);
        }}
        games={league?.results.length ?? 0}
      />
      {fatal ? (
        <div className="banner">{fatal}</div>
      ) : showLeague && league ? (
        <div className="grid league-grid">
          <div className="col col-main">
            <section className="card chart-card">
              <div className="chart-head">
                <div>
                  <h2 className="card-title">Ratings</h2>
                  <div className="chart-sub">
                    The game server&apos;s rating of each of its {league.bots.length} bots after every rated league game, as each game&apos;s rating stamp says.
                  </div>
                </div>
                {lastResult && (
                  <div className="last-game num" key={lastResult.seq}>
                    <span className="label">Game {lastResult.seq}</span>
                    <span>
                      {lastResult.bot.length}p {lastResult.podkidnoy ? 'podkidnoy' : 'classic'}
                      {lastResult.fool >= 0 && league.bots[lastResult.bot[lastResult.fool]] && (
                        <>
                          {' '}&middot; fool <b>{botName(league.bots[lastResult.bot[lastResult.fool]])}</b>{' '}
                          <span className="neg">{lastResult.delta[lastResult.fool]}</span>
                        </>
                      )}
                    </span>
                  </div>
                )}
              </div>
              <Families bots={league.bots} hidden={hidden} onToggle={toggleFamily} />
              <EloChart bots={league.bots} results={league.results} series={series} flash={flash} focus={focus} hidden={hidden} onHover={setHoverBot} />
            </section>
            <div className="section-head">
              <h2 className="card-title">At the tables now</h2>
              <span className="faint num">{liveTables.length} tables</span>
            </div>
            {liveTables.length === 0 ? (
              <div className="card empty-grid">{state === 'open' ? 'The league is seating its first tables...' : 'Connecting to birdseye...'}</div>
            ) : (
              <TableGrid tables={liveTables} compact={true} now={now} onSelect={select} />
            )}
          </div>
          <div className="col col-side">
            <Standings bots={league.bots} flash={flash} focus={focus} hover={hoverBot} onFocus={setFocus} now={now} />
          </div>
        </div>
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
              <TableGrid tables={shown} compact={compact} now={now} onSelect={select} />
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
