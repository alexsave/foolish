'use client';

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  EVERYONE,
  getEveryone,
  getEveryoneRegions,
  getOps,
  getRegions,
  getUser,
  getUsers,
  openFeed,
  AF_REJECTED,
  WireMismatch,
  type FeedState,
  type TypeIdx,
  type Everyone,
  type UserDetail,
  type WActivity,
  type WOps,
  type WRegion,
  type WRegionProgress,
  type WUser,
} from './api';
import { TopBar } from './components/TopBar';
import { StatsCard } from './components/StatsCard';
import { Regions } from './components/Regions';
import { MapView, type FlyTarget } from './components/MapView';
import { Feed } from './components/Feed';
import { Ops } from './components/Ops';

const USER_KEY = 'adventurer.user';
const TYPE_KEY = 'adventurer.type';
const FEED_CAP = 200;
const SPARK_N = 60;
const LIST_RETRY_MS = 2000;

// Polls `load` until it answers, then stops; a failure retries every LIST_RETRY_MS. The hook
// reruns when `key` changes (the user id, say) and ignores answers to a stale key.
function useRetrying<T>(load: (signal: AbortSignal) => Promise<T>, key: unknown, onFatal: (e: Error) => void) {
  const [data, setData] = useState<T | null>(null);
  const [failing, setFailing] = useState(false);
  const [bump, setBump] = useState(0);
  const loadRef = useRef(load);
  loadRef.current = load;
  const refresh = useCallback(() => setBump((b) => b + 1), []);

  useEffect(() => {
    const ac = new AbortController();
    let timer: ReturnType<typeof setTimeout> | null = null;
    let live = true;
    const attempt = async () => {
      try {
        const d = await loadRef.current(ac.signal);
        if (!live) return;
        setData(d);
        setFailing(false);
      } catch (e) {
        if (!live || ac.signal.aborted) return;
        if (e instanceof WireMismatch) {
          onFatal(e);
          return;
        }
        setFailing(true);
        timer = setTimeout(attempt, LIST_RETRY_MS);
      }
    };
    attempt();
    return () => {
      live = false;
      ac.abort();
      if (timer) clearTimeout(timer);
    };
  }, [key, bump]);

  return { data, failing, refresh };
}

function readStored(key: string): string | null {
  try {
    return window.localStorage.getItem(key);
  } catch {
    return null;
  }
}

function writeStored(key: string, v: string): void {
  try {
    window.localStorage.setItem(key, v);
  } catch {
    // private mode or blocked storage: the choice just does not survive a reload
  }
}

export function App() {
  const [fatal, setFatal] = useState<string | null>(null);
  const onFatal = useCallback((e: Error) => setFatal(e.message), []);

  const users = useRetrying<WUser[]>(getUsers, 'users', onFatal);
  const regions = useRetrying<WRegion[]>(getRegions, 'regions', onFatal);
  const opsFirst = useRetrying<WOps>(getOps, 'ops', onFatal);

  // The selected user: the stored one, else the one with the most activities once the list is in.
  const [userId, setUserId] = useState<number | null>(null);
  useEffect(() => {
    if (userId !== null || !users.data || users.data.length === 0) return;
    // ?user=<id> in the URL (0: everyone) beats the stored choice: a link to a map.
    const fromUrl = new URLSearchParams(window.location.search).get('user');
    if (fromUrl !== null && /^\d+$/.test(fromUrl)) {
      const id = Number(fromUrl);
      if (id === EVERYONE || users.data.some((u) => u.id === id)) {
        setUserId(id);
        return;
      }
    }
    const stored = readStored(USER_KEY);
    const found = stored !== null ? users.data.find((u) => u.id === Number(stored)) : undefined;
    if (found) {
      setUserId(found.id);
      return;
    }
    // The explorer with the most activities: an explorer seeks roads it has not seen, so its map
    // visibly grows on a live page; a walker re-walks its block. Else whoever has the most.
    let best = users.data[0];
    for (const u of users.data) if (u.nActivities > best.nActivities) best = u;
    let explorer: WUser | null = null;
    for (const u of users.data) if (u.kind === 2 && (!explorer || u.nActivities > explorer.nActivities)) explorer = u;
    setUserId(explorer && explorer.nActivities > 0 ? explorer.id : best.id);
  }, [users.data, userId]);

  const selectUser = useCallback((id: number) => {
    setUserId(id);
    writeStored(USER_KEY, String(id));
  }, []);

  // Storage is read after mount so the server-rendered shell and the first client render agree.
  const [type, setTypeState] = useState<TypeIdx>(2);
  useEffect(() => {
    const stored = readStored(TYPE_KEY);
    if (stored === null) return;
    const t = Number(stored);
    if (t === 0 || t === 1 || t === 2) setTypeState(t);
  }, []);
  const setType = useCallback((t: TypeIdx) => {
    setTypeState(t);
    writeStored(TYPE_KEY, String(t));
  }, []);

  const everyone = userId === EVERYONE;
  const userLoader = useCallback(
    (signal: AbortSignal) => (userId === null || userId === EVERYONE ? Promise.reject(new Error('no user')) : getUser(userId, signal)),
    [userId],
  );
  const detail = useRetrying<UserDetail>(userLoader, userId ?? 'none', onFatal);
  // The composite: its totals and top roads, and what anyone has per region (the Regions panel's rows).
  const everyoneLoader = useCallback(
    (signal: AbortSignal) => (everyone ? getEveryone(signal) : Promise.reject(new Error('not everyone'))),
    [everyone],
  );
  const everyoneData = useRetrying<Everyone>(everyoneLoader, everyone ? 'everyone' : 'none', onFatal);
  const everyoneRegionsLoader = useCallback(
    (signal: AbortSignal) => (everyone ? getEveryoneRegions(signal) : Promise.reject(new Error('not everyone'))),
    [everyone],
  );
  const everyoneRegions = useRetrying<WRegionProgress[]>(everyoneRegionsLoader, everyone ? 'everyone-regions' : 'none', onFatal);

  const usersById = useMemo(() => {
    const m = new Map<number, WUser>();
    for (const u of users.data ?? []) m.set(u.id, u);
    return m;
  }, [users.data]);
  const regionsById = useMemo(() => {
    const m = new Map<number, WRegion>();
    for (const r of regions.data ?? []) m.set(r.id, r);
    return m;
  }, [regions.data]);
  const user = userId !== null ? usersById.get(userId) ?? null : null;

  // ---- the feed --------------------------------------------------------------------------------
  const [feedState, setFeedState] = useState<FeedState>('connecting');
  const [activities, setActivities] = useState<WActivity[]>([]);
  const [ops, setOps] = useState<WOps | null>(null);
  const [spark, setSpark] = useState<number[]>([]);
  const [paused, setPaused] = useState(false);
  const pausedRef = useRef(false);
  pausedRef.current = paused;
  const pending = useRef<WActivity[]>([]);
  // The selected user's coverage version, bumped by the feed so the map refetches its tiles.
  const [coverageVersion, setCoverageVersion] = useState(0);
  const userIdRef = useRef<number | null>(null);
  userIdRef.current = userId;
  const refreshDetail = detail.refresh;
  const refreshEveryone = everyoneData.refresh;
  const refreshEveryoneRegions = everyoneRegions.refresh;
  const refreshTimer = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    const close = openFeed({
      onState: setFeedState,
      onError: (e) => {
        if (e instanceof WireMismatch) setFatal(e.message);
      },
      onOps: (o) => {
        setOps(o);
        setSpark((s) => {
          const next = s.length >= SPARK_N ? s.slice(s.length - SPARK_N + 1) : s.slice();
          next.push(o.actPerS);
          return next;
        });
      },
      onActivity: (a) => {
        if (pausedRef.current) {
          pending.current.push(a);
          if (pending.current.length > FEED_CAP) pending.current.splice(0, pending.current.length - FEED_CAP);
        } else {
          setActivities((rows) => {
            const next = [a, ...rows];
            return next.length > FEED_CAP ? next.slice(0, FEED_CAP) : next;
          });
        }
        // Everyone's map grows with every user's upload; a user's with its own.
        const all = userIdRef.current === EVERYONE;
        if ((all || a.user === userIdRef.current) && !(a.flags & AF_REJECTED)) {
          setCoverageVersion((v) => v + 1);
          // The stats card follows the tiles, a little later and coalesced under a burst.
          if (refreshTimer.current) clearTimeout(refreshTimer.current);
          refreshTimer.current = setTimeout(() => {
            refreshTimer.current = null;
            if (all) {
              refreshEveryone();
              refreshEveryoneRegions();
            } else refreshDetail();
          }, 1500);
        }
      },
    });
    return () => {
      close();
      if (refreshTimer.current) clearTimeout(refreshTimer.current);
    };
  }, [refreshDetail, refreshEveryone, refreshEveryoneRegions]);

  const togglePause = useCallback(() => {
    setPaused((p) => {
      if (p && pending.current.length) {
        const buffered = pending.current.reverse();
        pending.current = [];
        setActivities((rows) => [...buffered, ...rows].slice(0, FEED_CAP));
      }
      return !p;
    });
  }, []);

  // Where the map should go next: a region click or the Home button sets it.
  const [fly, setFly] = useState<FlyTarget | null>(null);
  const flyToRegion = useCallback((r: WRegion) => {
    setFly({ kind: 'bbox', minLat: r.minLat, minLon: r.minLon, maxLat: r.maxLat, maxLon: r.maxLon, seq: Date.now() });
  }, []);
  const flyHome = useCallback(() => {
    if (!user) return;
    setFly({ kind: 'point', lat: user.homeLat, lon: user.homeLon, zoom: 14, seq: Date.now() });
  }, [user]);
  const flyToPoint = useCallback((lat: number, lon: number) => {
    setFly({ kind: 'point', lat, lon, zoom: 16, seq: Date.now() });
  }, []);

  const live = ops ?? opsFirst.data;
  const connecting = !users.data || !regions.data;

  return (
    <div className="app">
      <TopBar
        users={users.data ?? []}
        selected={userId}
        onSelect={selectUser}
        feedState={feedState}
        connecting={connecting}
      />
      {fatal ? (
        <div className="banner">{fatal}</div>
      ) : (
        <div className="grid">
          <div className="col">
            <StatsCard
              user={user}
              detail={detail.data && detail.data.stats.id === userId ? detail.data : null}
              everyone={everyone ? everyoneData.data : null}
              type={type}
              regionsById={regionsById}
              onFly={flyToPoint}
            />
            <Regions
              type={type}
              onType={setType}
              progress={everyone ? everyoneRegions.data : detail.data && detail.data.stats.id === userId ? detail.data.progress : null}
              everyone={everyone}
              regionsById={regionsById}
              usersById={usersById}
              onFly={flyToRegion}
              onFatal={onFatal}
            />
          </div>
          <div className="col col-center">
            <MapView
              userId={userId}
              type={type}
              coverageVersion={coverageVersion}
              regions={regions.data}
              fly={fly}
              onHome={flyHome}
              connecting={connecting}
            />
          </div>
          <div className="col">
            <Feed
              rows={activities}
              usersById={usersById}
              onSelectUser={selectUser}
              rate={live?.actPerS ?? 0}
              paused={paused}
              onTogglePause={togglePause}
              feedState={feedState}
            />
            <Ops ops={live} spark={spark} />
          </div>
        </div>
      )}
    </div>
  );
}
