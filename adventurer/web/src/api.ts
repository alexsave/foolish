// api.ts - the browser's side of wire.h: the binary endpoints read and the feed heard through the
// platform's client (c_base/client: one client in wasm over the anonymous socket, GET /ws/public),
// each record decoded by the generated readers (structgen's, from the C structs themselves).
// Nothing here knows a byte offset, of the protocol or of a record: a record is read by its
// generated reader at the offset its generated size says, and a page built from another layout
// refuses the bytes.
import { LAYOUT_HASH } from './gen/layout_hash';
import {
  ADV_W_KIND_ACTIVITY,
  ADV_W_KIND_OPS,
  ADV_W_V,
  adv_w_activity_SIZE,
  adv_w_everyone_SIZE,
  adv_w_leaders_SIZE,
  adv_w_list_SIZE,
  adv_w_ops_SIZE,
  adv_w_region_progress_SIZE,
  adv_w_region_SIZE,
  adv_w_top_road_SIZE,
  adv_w_user_SIZE,
  adv_w_user_stats_SIZE,
  memOf,
  readadv_w_activity,
  readadv_w_everyone,
  readadv_w_leaders,
  readadv_w_list,
  readadv_w_ops,
  readadv_w_region,
  readadv_w_region_progress,
  readadv_w_top_road,
  readadv_w_user,
  readadv_w_user_stats,
  type adv_w_activity_Snap,
  type adv_w_everyone_Snap,
  type adv_w_leader_Snap,
  type adv_w_ops_Snap,
  type adv_w_region_progress_Snap,
  type adv_w_region_Snap,
  type adv_w_top_road_Snap,
  type adv_w_user_Snap,
  type adv_w_user_stats_Snap,
  type Mem,
} from './gen/wire';
import { CbClient, CBC_ROOM } from './gen/cbase/socket.ts';

// The records as the page holds them: the generated snapshots, with the 64-bit fields (an
// instant in ms, a byte count) widened from bigint to number, which holds them for a few
// hundred thousand years.
type Widened<T, K extends keyof T> = Omit<T, K> & { readonly [P in K]: number };
export type WUser = adv_w_user_Snap;
export type WRegion = adv_w_region_Snap;
export type WRegionProgress = adv_w_region_progress_Snap;
export type WLeader = adv_w_leader_Snap;
export type WTopRoad = adv_w_top_road_Snap;
export type WEveryone = adv_w_everyone_Snap;
export type WUserStats = Widened<adv_w_user_stats_Snap, 'firstActivityMs' | 'lastActivityMs'>;
export type WActivity = Widened<adv_w_activity_Snap, 'atMs'>;
export type WOps = Widened<adv_w_ops_Snap, 'atMs' | 'journalBytes'>;

const readWUserStats = (m: Mem, p: number): WUserStats => {
  const r = readadv_w_user_stats(m, p);
  return { ...r, firstActivityMs: Number(r.firstActivityMs), lastActivityMs: Number(r.lastActivityMs) };
};
const readWActivity = (m: Mem, p: number): WActivity => {
  const r = readadv_w_activity(m, p);
  return { ...r, atMs: Number(r.atMs) };
};
const readWOps = (m: Mem, p: number): WOps => {
  const r = readadv_w_ops(m, p);
  return { ...r, atMs: Number(r.atMs), journalBytes: Number(r.journalBytes) };
};

// The user id that is nobody: the composite of everyone's coverage (GET /adv/everyone).
export const EVERYONE = 0;

// The C server's origin. The page is served by Next, the data comes from here, cross-origin
// (the server answers CORS), so every URL the browser or MapLibre fetches is absolute.
export const SERVER = (process.env.NEXT_PUBLIC_ADV_SERVER ?? 'http://127.0.0.1:8711').replace(/\/+$/, '');

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
  if (hash !== LAYOUT_HASH) throw new WireMismatch();
}

// A list answer (adv_w_list, then n records of `size` bytes each): the head's hash checked, the
// records read by `read` at the offsets the generated sizes give, never past the bytes received.
function readList<T>(buf: ArrayBuffer, size: number, read: (m: Mem, p: number) => T): T[] {
  if (buf.byteLength < adv_w_list_SIZE) throw new Error('list: short answer');
  const m = memOf(buf);
  const head = readadv_w_list(m, 0);
  checkHash(head.hash);
  const n = Math.min(head.n, Math.floor((buf.byteLength - adv_w_list_SIZE) / size));
  const out: T[] = [];
  for (let i = 0; i < n; i++) out.push(read(m, adv_w_list_SIZE + i * size));
  return out;
}

// THE CLIENT: one for the page, anonymous (it never signs in), on the server's origin. It reads over
// HTTP (an anonymous socket carries no requests) with the platform's retries, and holds the feed room
// on the socket. The feed's handlers are whoever has the feed open (openFeed).
let feed: FeedHandlers | null = null;
let opened: Promise<CbClient> | null = null;
function client(): Promise<CbClient> {
  opened ??= CbClient.open({
    wasm: '/cbase_client.wasm',
    server: SERVER,
    anonymous: true,
    // 'open' is the feed room's SUB going live, not the socket's READY.
    onState: (s) => { if (s !== 'open') feed?.onState(s); },
    onSubLive: () => feed?.onState('open'),
    onSubRefused: (_what, _room, status) => feed?.onError(new Error(`feed: the server refused the subscription (${status})`)),
    onPush: (kind, frame) => feedPush(kind, frame),
  });
  return opened;
}

// A read through the client; an aborted one rejects at once and its answer is dropped.
async function fetchBytes(path: string, signal?: AbortSignal): Promise<ArrayBuffer> {
  const c = await client();
  const got = c.get(path);
  if (!signal) return got;
  if (signal.aborted) throw new DOMException('aborted', 'AbortError');
  return new Promise((resolve, reject) => {
    const abort = () => reject(new DOMException('aborted', 'AbortError'));
    signal.addEventListener('abort', abort, { once: true });
    got.then(resolve, reject).finally(() => signal.removeEventListener('abort', abort));
  });
}

export async function getUsers(signal?: AbortSignal): Promise<WUser[]> {
  const buf = await fetchBytes('/adv/users', signal);
  return readList(buf, adv_w_user_SIZE, readadv_w_user);
}

export async function getRegions(signal?: AbortSignal): Promise<WRegion[]> {
  const buf = await fetchBytes('/adv/regions', signal);
  return readList(buf, adv_w_region_SIZE, readadv_w_region);
}

export interface UserDetail {
  stats: WUserStats;
  progress: WRegionProgress[];
}

export async function getUser(id: number, signal?: AbortSignal): Promise<UserDetail> {
  const buf = await fetchBytes(`/adv/user/${id}`, signal);
  if (buf.byteLength < adv_w_user_stats_SIZE) throw new Error('user: short answer');
  const m = memOf(buf);
  const stats = readWUserStats(m, 0);
  checkHash(stats.hash);
  const n = Math.min(stats.nRegions, Math.floor((buf.byteLength - adv_w_user_stats_SIZE) / adv_w_region_progress_SIZE));
  const progress: WRegionProgress[] = [];
  for (let i = 0; i < n; i++) progress.push(readadv_w_region_progress(m, adv_w_user_stats_SIZE + i * adv_w_region_progress_SIZE));
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
  if (buf.byteLength < adv_w_leaders_SIZE) throw new Error('leaders: short answer');
  const head = readadv_w_leaders(memOf(buf), 0);
  checkHash(head.hash);
  // Nine records per table, index type*3+rank, into rows per type.
  const rows = (flat: readonly WLeader[]): WLeader[][] => [0, 1, 2].map((t) => flat.slice(t * 3, t * 3 + 3));
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
  if (buf.byteLength < adv_w_everyone_SIZE) throw new Error('everyone: short answer');
  const m = memOf(buf);
  const stats = readadv_w_everyone(m, 0);
  checkHash(stats.hash);
  const n = Math.min(stats.nTop, Math.floor((buf.byteLength - adv_w_everyone_SIZE) / adv_w_top_road_SIZE));
  const top: WTopRoad[] = [];
  for (let i = 0; i < n; i++) top.push(readadv_w_top_road(m, adv_w_everyone_SIZE + i * adv_w_top_road_SIZE));
  return { stats, top };
}
export async function getEveryoneRegions(signal?: AbortSignal): Promise<WRegionProgress[]> {
  const buf = await fetchBytes('/adv/everyone/regions', signal);
  return readList(buf, adv_w_region_progress_SIZE, readadv_w_region_progress);
}

export async function getOps(signal?: AbortSignal): Promise<WOps> {
  const buf = await fetchBytes('/adv/ops', signal);
  if (buf.byteLength < adv_w_ops_SIZE) throw new Error('ops: short answer');
  const ops = readWOps(memOf(buf), 0);
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

// THE FEED ROOM (server feed.h ADV_FEED_ROOM), held on the platform's anonymous socket.
const FEED_ROOM = 'feed';

// A feed push, as the client hands it: the whole frame, [SF_V][kind] then the record, which carries
// those two bytes as its own first fields (wire.h), so it is read from 0.
function feedPush(kind: number, frame: ArrayBuffer): void {
  const h = feed;
  if (!h) return;
  try {
    const m = memOf(frame);
    if (kind === ADV_W_KIND_ACTIVITY && frame.byteLength >= adv_w_activity_SIZE) {
      const a = readWActivity(m, 0);
      if (a.v === ADV_W_V) h.onActivity(a);
    } else if (kind === ADV_W_KIND_OPS && frame.byteLength >= adv_w_ops_SIZE) {
      const ops = readWOps(m, 0);
      if (ops.v !== ADV_W_V) return;
      checkHash(ops.hash);
      h.onOps(ops);
    }
  } catch (e) {
    h.onError(e instanceof Error ? e : new Error(String(e)));
    if (e instanceof WireMismatch) stopFeed?.();
  }
}
let stopFeed: (() => void) | null = null;

// Holds the feed room until the returned closer is called. The platform's client keeps it: it says
// HELLO, SUBs the room after READY, pings, and on a close reopens after its jittered wait and holds
// the room again; onState says 'open' when the room's SUB goes live.
export function openFeed(h: FeedHandlers): () => void {
  feed = h;
  let release: (() => void) | null = null;
  let stopped = false;
  const stop = () => {
    if (stopped) return;
    stopped = true;
    if (feed === h) feed = null;
    if (stopFeed === stop) stopFeed = null;
    release?.();
  };
  stopFeed = stop;
  h.onState('connecting');
  client().then(
    (c) => { if (!stopped) release = c.hold(CBC_ROOM, FEED_ROOM); },
    (e) => h.onError(e instanceof Error ? e : new Error(String(e))),
  );
  return stop;
}

// ---- formatting ------------------------------------------------------------------------------
const M_PER_MILE = 1609.344;

// A length in km for a tight card, the unit left to the caller: "41.2", "412", "1.68k", "41.5k".
export function kmNumber(m: number): string {
  const k = m / 1000;
  if (k < 1000) return k < 100 ? k.toFixed(1) : k.toFixed(0);
  return `${(k / 1000).toFixed(k < 10000 ? 2 : 1)}k`;
}
// A count for a tight card: "22.3k", "554k", "1.2M".
export function countCompact(n: number): string {
  if (n < 10000) return int(n);
  if (n < 1000000) return `${(n / 1000).toFixed(n < 100000 ? 1 : 0)}k`;
  return `${(n / 1000000).toFixed(1)}M`;
}

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
