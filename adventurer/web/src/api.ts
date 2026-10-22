// api.ts - the browser's side of wire.h: fetch the binary endpoints, decode them with the
// generated decoder, open the feed WebSocket. Nothing here knows a byte offset.
import {
  readList,
  readWActivity,
  readWLeaders,
  readWOps,
  readWRegion,
  readWEveryone,
  readWRegionProgress,
  readWTopRoad,
  readWUser,
  readWUserStats,
  SIZE,
  WIRE_HASH,
  type WActivity,
  type WEveryone,
  type WLeader,
  type WOps,
  type WRegion,
  type WRegionProgress,
  type WTopRoad,
  type WUser,
  type WUserStats,
} from './gen/wire';

export type {
  WActivity,
  WEveryone,
  WLeader,
  WOps,
  WRegion,
  WRegionProgress,
  WTopRoad,
  WUser,
  WUserStats,
};

// The user id that is nobody: the composite of everyone's coverage (GET /adv/everyone).
export const EVERYONE = 0;

// The C server's origin. The page is served by Next, the data comes from here, cross-origin
// (the server answers CORS), so every URL the browser or MapLibre fetches is absolute.
export const SERVER = (process.env.NEXT_PUBLIC_ADV_SERVER ?? 'http://127.0.0.1:8711').replace(/\/+$/, '');
const WS_SERVER = SERVER.replace(/^http/, 'ws');

// ADV_T_* of graph.h: the index into every per-type triple.
export type TypeIdx = 0 | 1 | 2;
export const TYPE_LABEL: readonly string[] = ['Bike', 'Foot', 'Combined'];
export const TYPE_PARAM: readonly string[] = ['bike', 'foot', 'all'];

// ADV_UK_* of wire.h.
export const USER_KIND: readonly string[] = ['', 'commuter', 'explorer', 'runner', 'walker', 'tourist'];
// ADV_ACT_* of trace.h.
export const ACT_TYPE: readonly string[] = ['', 'bike', 'run', 'walk'];
// ADV_MATCH_RJ_* of match.h.
export const REJECT_REASON: readonly string[] = ['', 'short', 'coords', 'time', 'teleport', 'budget'];
// ADV_AF_* of wire.h.
export const AF_BACKFILL = 0x1;
export const AF_REJECTED = 0x2;
export const AF_TRUTH = 0x4;
// ADV_HC_* of graph.h, the "c" tag of every tile feature.
export const HIGHWAY_CLASS: readonly string[] = [
  'motorway', 'trunk', 'primary', 'secondary', 'tertiary', 'residential', 'unclassified', 'service',
  'living street', 'track', 'cycleway', 'footway', 'path', 'pedestrian', 'steps', 'other',
];
// ADV_ACH_PCT of wire.h.
export const ACH_PCT: readonly number[] = [25, 50, 75, 90, 99];

// A page built from another layout refuses the bytes: the caller shows one banner and stops.
export class WireMismatch extends Error {
  constructor() {
    super('decoder out of date: rebuild (make -C adventurer/server)');
    this.name = 'WireMismatch';
  }
}

function checkHash(hash: number): void {
  if (hash !== WIRE_HASH) throw new WireMismatch();
}

// readList throws its own message on a mismatch; fold it into the one class.
function wrapList<T>(f: () => T): T {
  try {
    return f();
  } catch (e) {
    if (e instanceof Error && /wire layout mismatch/.test(e.message)) throw new WireMismatch();
    throw e;
  }
}

async function fetchBytes(path: string, signal?: AbortSignal): Promise<ArrayBuffer> {
  const res = await fetch(SERVER + path, { signal, cache: 'no-store' });
  if (!res.ok) throw new Error(`${path}: HTTP ${res.status}`);
  return res.arrayBuffer();
}

export async function getUsers(signal?: AbortSignal): Promise<WUser[]> {
  const buf = await fetchBytes('/adv/users', signal);
  return wrapList(() => readList(buf, SIZE.WUser, readWUser));
}

export async function getRegions(signal?: AbortSignal): Promise<WRegion[]> {
  const buf = await fetchBytes('/adv/regions', signal);
  return wrapList(() => readList(buf, SIZE.WRegion, readWRegion));
}

export interface UserDetail {
  stats: WUserStats;
  progress: WRegionProgress[];
}

export async function getUser(id: number, signal?: AbortSignal): Promise<UserDetail> {
  const buf = await fetchBytes(`/adv/user/${id}`, signal);
  if (buf.byteLength < SIZE.WUserStats) throw new Error('user: short answer');
  const dv = new DataView(buf);
  const stats = readWUserStats(dv, 0);
  checkHash(stats.hash);
  const n = Math.min(stats.nRegions, Math.floor((buf.byteLength - SIZE.WUserStats) / SIZE.WRegionProgress));
  const progress: WRegionProgress[] = [];
  for (let i = 0; i < n; i++) progress.push(readWRegionProgress(dv, SIZE.WUserStats + i * SIZE.WRegionProgress));
  return { stats, progress };
}

// Leaders per type: month[type][rank], year[type][rank]; an empty slot has user 0.
export interface Leaders {
  region: number;
  month: WLeader[][];
  year: WLeader[][];
}

export async function getLeaders(region: number, signal?: AbortSignal): Promise<Leaders> {
  const buf = await fetchBytes(`/adv/leaders/${region}`, signal);
  if (buf.byteLength < SIZE.WLeaders) throw new Error('leaders: short answer');
  const head = readWLeaders(new DataView(buf), 0);
  checkHash(head.hash);
  // Nine records per table, index type*3+rank, into rows per type.
  const rows = (flat: WLeader[]): WLeader[][] => [0, 1, 2].map((t) => flat.slice(t * 3, t * 3 + 3));
  return { region: head.region, month: rows(head.month), year: rows(head.year) };
}

// Everyone's coverage: the totals per type and the most-travelled roads (ADV_TOP_N per type, in
// type order), and per region what anyone has (the shape of a user's progress, achievements and
// ranks left zero).
export interface Everyone {
  stats: WEveryone;
  top: WTopRoad[];
}
export async function getEveryone(signal?: AbortSignal): Promise<Everyone> {
  const buf = await fetchBytes('/adv/everyone', signal);
  const dv = new DataView(buf);
  const stats = readWEveryone(dv, 0);
  checkHash(stats.hash);
  const top: WTopRoad[] = [];
  for (let i = 0; i < stats.nTop; i++) top.push(readWTopRoad(dv, SIZE.WEveryone + i * SIZE.WTopRoad));
  return { stats, top };
}
export async function getEveryoneRegions(signal?: AbortSignal): Promise<WRegionProgress[]> {
  const buf = await fetchBytes('/adv/everyone/regions', signal);
  return wrapList(() => readList(buf, SIZE.WRegionProgress, readWRegionProgress));
}

export async function getOps(signal?: AbortSignal): Promise<WOps> {
  const buf = await fetchBytes('/adv/ops', signal);
  if (buf.byteLength < SIZE.WOps) throw new Error('ops: short answer');
  const ops = readWOps(new DataView(buf), 0);
  checkHash(ops.hash);
  return ops;
}

// Tile URL templates for MapLibre. `v` is the coverage version: the server's ETag changes with
// it, and a new template makes MapLibre drop its cache and refetch.
export function baseTileUrl(): string {
  return `${SERVER}/adv/base/{z}/{x}/{y}.mvt`;
}

export function userTileUrl(user: number, type: TypeIdx, version: number): string {
  return `${SERVER}/adv/tile/${user}/{z}/{x}/{y}.mvt?t=${TYPE_PARAM[type]}&v=${version}`;
}
export function everyoneTileUrl(type: TypeIdx, version: number): string {
  return `${SERVER}/adv/everyone/{z}/{x}/{y}.mvt?t=${TYPE_PARAM[type]}&v=${version}`;
}

// ---- the feed ---------------------------------------------------------------------------------
export type FeedState = 'connecting' | 'open' | 'closed';

export interface FeedHandlers {
  onActivity: (a: WActivity) => void;
  onOps: (o: WOps) => void;
  onState: (s: FeedState) => void;
  onError: (e: Error) => void;
}

// Opens /adv/feed and keeps it open: a close reconnects with backoff (1 s doubling to 15 s,
// reset by a frame). Returns the closer.
export function openFeed(h: FeedHandlers): () => void {
  let ws: WebSocket | null = null;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let delay = 1000;
  let stopped = false;

  const connect = () => {
    if (stopped) return;
    h.onState('connecting');
    const sock = new WebSocket(`${WS_SERVER}/adv/feed`);
    sock.binaryType = 'arraybuffer';
    ws = sock;
    sock.onopen = () => {
      if (sock !== ws) return;
      delay = 1000;
      h.onState('open');
    };
    sock.onmessage = (ev: MessageEvent) => {
      if (sock !== ws || !(ev.data instanceof ArrayBuffer)) return;
      const buf = ev.data;
      if (buf.byteLength < 1) return;
      const dv = new DataView(buf);
      const kind = dv.getUint8(0);
      try {
        if (kind === 1 && buf.byteLength >= SIZE.WActivity) {
          h.onActivity(readWActivity(dv, 0));
        } else if (kind === 2 && buf.byteLength >= SIZE.WOps) {
          const ops = readWOps(dv, 0);
          checkHash(ops.hash);
          h.onOps(ops);
        }
      } catch (e) {
        h.onError(e instanceof Error ? e : new Error(String(e)));
        if (e instanceof WireMismatch) {
          stopped = true;
          sock.close();
        }
      }
    };
    sock.onclose = () => {
      if (sock !== ws) return;
      ws = null;
      h.onState('closed');
      if (stopped) return;
      timer = setTimeout(connect, delay);
      delay = Math.min(delay * 2, 15000);
    };
    sock.onerror = () => {
      // onclose follows and schedules the retry.
    };
  };

  connect();
  return () => {
    stopped = true;
    if (timer) clearTimeout(timer);
    if (ws) {
      const s = ws;
      ws = null;
      s.close();
    }
  };
}

// ---- formatting ------------------------------------------------------------------------------
const M_PER_MILE = 1609.344;

export function km(m: number, digits = 1): string {
  return (m / 1000).toLocaleString(undefined, { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

export function miles(m: number, digits = 1): string {
  return (m / M_PER_MILE).toLocaleString(undefined, { minimumFractionDigits: digits, maximumFractionDigits: digits });
}

// Wandrer's score: one point per unique mile.
export function points(uniqueM: number): string {
  return miles(uniqueM, 1);
}

export function pct(part: number, whole: number): number {
  return whole > 0 ? Math.min(100, (part / whole) * 100) : 0;
}

export function int(n: number): string {
  return n.toLocaleString();
}

// Microseconds shown as ms with two decimals, or as whole us under a millisecond.
export function usToText(us: number): string {
  if (us < 1000) return `${Math.round(us)} µs`;
  return `${(us / 1000).toFixed(2)} ms`;
}

export function msFromUs(us: number): string {
  return (us / 1000).toFixed(1);
}

export function mb(bytes: number): string {
  return (bytes / (1024 * 1024)).toFixed(1);
}

export function uptime(s: number): string {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m ${s % 60}s`;
}

export function dateText(ms: number): string {
  if (!ms) return '-';
  return new Date(ms).toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

export function ago(ms: number, now: number): string {
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 5) return 'now';
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h}h`;
  return `${Math.floor(h / 24)}d`;
}

// Degrees from the wire's int32 1e-7 degrees.
export function deg(v: number): number {
  return v / 1e7;
}
