// api.ts - the browser's side of wire.h: fetch the binary endpoints and decode them with the
// generated readers; open the platform's push socket (/ws) through the stream codec compiled to
// wasm (public/sf.wasm, bindings in gen/sf_wasm.ts) and decode every push the same way. Nothing
// here spells a byte offset: the readers are structgen's, the frames are stream_frame.h's.
import {
  memOf,
  readbe_w_detail,
  readbe_w_event,
  readbe_w_list,
  readbe_w_session,
  readbe_w_table,
  BE_F_EVENT,
  BE_F_OPS,
  BE_F_TABLE,
  type Mem,
  type be_w_card_Snap,
  type be_w_detail_Snap,
  type be_w_event_Snap,
  type be_w_table_Snap,
} from './gen/wire';
import { LAYOUT_HASH } from './gen/wire_hash';
import {
  cb_metrics_hdr_SIZE,
  readcb_metric_row,
  readcb_metrics_hdr,
  CB_METRICS_WIRE_VERSION,
  type cb_metric_row_Snap,
  type cb_metrics_hdr_Snap,
} from './gen/metrics';
import type { SfWasm } from './gen/sf_wasm';

export type WCard = be_w_card_Snap;
export type WDetail = be_w_detail_Snap;
export type WEvent = be_w_event_Snap;
export type WTable = be_w_table_Snap;
export type MetricRow = cb_metric_row_Snap;

// The platform's metrics report (c_base/core/src/metrics.h): the header and every row by name,
// the platform's own rows (requests, errors_5xx, http_conns, ws_conns) among them.
export interface Metrics {
  hdr: cb_metrics_hdr_Snap;
  rows: Map<string, MetricRow>;
}

// The rows follow the header back to back, each hdr.rowSize bytes (the platform's own sizeof).
function readMetrics(m: Mem, at: number): Metrics {
  const hdr = readcb_metrics_hdr(m, at);
  if (hdr.version !== CB_METRICS_WIRE_VERSION) throw new WireMismatch();
  const rows = new Map<string, MetricRow>();
  for (let i = 0; i < hdr.nRows; i++) {
    const r = readcb_metric_row(m, at + cb_metrics_hdr_SIZE + i * hdr.rowSize);
    rows.set(r.name, r);
  }
  return { hdr, rows };
}

// The birdseye process's origin. The page is served by Next, the data comes from here,
// cross-origin (the platform answers CORS), so every URL is absolute.
export const SERVER = (process.env.NEXT_PUBLIC_BE_SERVER ?? 'http://127.0.0.1:8812').replace(/\/+$/, '');
const WS_SERVER = SERVER.replace(/^http/, 'ws');

// netwire.h NW_SUB_GAME: a room on the hub. The one room birdseye publishes to is "all".
const NW_SUB_GAME = 2;
const ROOM = 'all';
const PING_MS = 30000;

// A page built from another layout refuses the bytes: the caller shows one banner and stops.
export class WireMismatch extends Error {
  constructor() {
    super('decoder out of date: rebuild (make -C birdseye/server)');
    this.name = 'WireMismatch';
  }
}

function checkHash(hash: number): void {
  if ((hash >>> 0) !== (LAYOUT_HASH >>> 0)) throw new WireMismatch();
}

async function fetchBytes(path: string, init?: RequestInit): Promise<ArrayBuffer> {
  const res = await fetch(SERVER + path, { cache: 'no-store', ...init });
  if (!res.ok) throw new Error(`${path}: HTTP ${res.status}`);
  return res.arrayBuffer();
}

export async function getTables(signal?: AbortSignal): Promise<WTable[]> {
  const m = memOf(await fetchBytes('/be/tables', { signal }));
  const head = readbe_w_list(m, 0);
  checkHash(head.hash);
  const out: WTable[] = [];
  let at = head.first;
  for (let i = 0; i < head.n; i++, at += head.size) out.push(readbe_w_table(m, at));
  return out;
}

export async function getDetail(gid: string, signal?: AbortSignal): Promise<WDetail> {
  const m = memOf(await fetchBytes(`/be/table/${encodeURIComponent(gid)}`, { signal }));
  return readbe_w_detail(m, 0);
}

export async function getMetrics(signal?: AbortSignal): Promise<Metrics> {
  return readMetrics(memOf(await fetchBytes('/metrics', { signal })), 0);
}

// A row's numbers with a default for a name the report does not carry (an older build).
export function metric(ms: Metrics | null, name: string): MetricRow | null {
  return ms?.rows.get(name) ?? null;
}
export function gauge(ms: Metrics | null, name: string): number {
  const r = metric(ms, name);
  return r ? Number(r.total) : 0;
}
export function perS(ms: Metrics | null, name: string): number {
  return metric(ms, name)?.perS ?? 0;
}

async function getSession(): Promise<string> {
  const m = memOf(await fetchBytes('/be/session', { method: 'POST' }));
  const s = readbe_w_session(m, 0);
  checkHash(s.hash);
  return s.token;
}

// ---- the stream --------------------------------------------------------------------------------
export type StreamState = 'connecting' | 'open' | 'closed';

export interface StreamHandlers {
  onTable: (t: WTable) => void;
  onEvent: (e: WEvent) => void;
  onOps: (o: Metrics) => void;
  onState: (s: StreamState) => void;
  onError: (e: Error) => void;
}

interface Codec {
  w: SfWasm;
  mem: Mem;
  inPtr: number;
  inCap: number;
  outPtr: number;
}

let codecPromise: Promise<Codec> | null = null;

function loadCodec(): Promise<Codec> {
  if (!codecPromise) {
    codecPromise = (async () => {
      const res = await fetch('/sf.wasm', { cache: 'no-store' });
      if (!res.ok) throw new Error(`sf.wasm: HTTP ${res.status}`);
      const { instance } = await WebAssembly.instantiate(await res.arrayBuffer(), {});
      const w = instance.exports as unknown as SfWasm;
      return { w, mem: memOf(w.memory.buffer), inPtr: w.sfw_in(), inCap: w.sfw_in_cap(), outPtr: w.sfw_out() };
    })();
  }
  return codecPromise;
}

const utf8 = new TextEncoder();

// The codec's `out` after a writer, as the bytes to send.
function outFrame(c: Codec, len: number): Uint8Array {
  if (len < 0) throw new Error('stream frame refused by the codec');
  return c.mem.u8.slice(c.outPtr, c.outPtr + len);
}

function putIn(c: Codec, bytes: Uint8Array): number {
  if (bytes.length > c.inCap) return -1;
  c.mem.u8.set(bytes, c.inPtr);
  return bytes.length;
}

// Opens the stream and keeps it open: a close reconnects with backoff (1 s doubling to 15 s,
// reset by a frame). The token comes from POST /be/session each time. Returns the closer.
export function openStream(h: StreamHandlers): () => void {
  let ws: WebSocket | null = null;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let ping: ReturnType<typeof setInterval> | null = null;
  let delay = 1000;
  let stopped = false;

  const schedule = () => {
    if (stopped) return;
    timer = setTimeout(connect, delay);
    delay = Math.min(delay * 2, 15000);
  };

  const connect = async () => {
    if (stopped) return;
    h.onState('connecting');
    let codec: Codec;
    let token: string;
    try {
      codec = await loadCodec();
      token = await getSession();
    } catch (e) {
      h.onError(e instanceof Error ? e : new Error(String(e)));
      if (e instanceof WireMismatch) {
        stopped = true;
        return;
      }
      h.onState('closed');
      schedule();
      return;
    }
    if (stopped) return;
    const sock = new WebSocket(`${WS_SERVER}/ws`);
    sock.binaryType = 'arraybuffer';
    ws = sock;
    let ready = false;
    sock.onopen = () => {
      if (sock !== ws) return;
      sock.send(outFrame(codec, codec.w.sfw_hello(putIn(codec, utf8.encode(token)))));
    };
    sock.onmessage = (ev: MessageEvent) => {
      if (sock !== ws || !(ev.data instanceof ArrayBuffer)) return;
      const n = putIn(codec, new Uint8Array(ev.data));
      if (n < 0) return;
      try {
        const kind = codec.w.sfw_kind(n);
        if (kind > 0) {
          // The socket's own frame: READY once, then the SUB's REPLY, PONGs.
          if (!ready) {
            ready = true;
            delay = 1000;
            sock.send(outFrame(codec, codec.w.sfw_sub(1, NW_SUB_GAME, putIn(codec, utf8.encode(ROOM)), 0)));
            h.onState('open');
          }
          return;
        }
        if (kind < 0) return;
        const body = codec.w.sfw_push_body();
        const at = codec.inPtr + body;
        switch (codec.w.sfw_push_kind(n)) {
          case BE_F_TABLE:
            h.onTable(readbe_w_table(codec.mem, at));
            break;
          case BE_F_EVENT:
            h.onEvent(readbe_w_event(codec.mem, at));
            break;
          case BE_F_OPS:
            h.onOps(readMetrics(codec.mem, at));
            break;
          default:
            break;
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
      if (ping) clearInterval(ping);
      ping = null;
      h.onState('closed');
      schedule();
    };
    sock.onerror = () => {
      // onclose follows and schedules the retry.
    };
    ping = setInterval(() => {
      if (sock === ws && sock.readyState === WebSocket.OPEN) sock.send(outFrame(codec, codec.w.sfw_ping()));
    }, PING_MS);
  };

  connect();
  return () => {
    stopped = true;
    if (timer) clearTimeout(timer);
    if (ping) clearInterval(ping);
    if (ws) {
      const s = ws;
      ws = null;
      s.close();
    }
  };
}

// ---- the game's vocabulary -------------------------------------------------------------------
// card.h: suits 0 spades, 1 hearts, 2 clubs, 3 diamonds; values 1..13 with the ace 13, so the
// rank shown is the value plus one (value 9 is a ten, 10 a jack, 13 an ace).
export const SUIT_GLYPH: readonly string[] = ['♠', '♥', '♣', '♦'];
export const SUIT_RED: readonly boolean[] = [false, true, false, true];

export function rankText(value: number): string {
  if (value === 13) return 'A';
  if (value === 12) return 'K';
  if (value === 11) return 'Q';
  if (value === 10) return 'J';
  return String(value + 1);
}

export function cardIsNone(c: WCard): boolean {
  return c.suit === -2 && c.value === -2;
}
export function cardIsHidden(c: WCard): boolean {
  return c.suit === -1 && c.value === -1;
}

// wire.h BE_EV_*, as the feed words them.
export const EVENT_WORD: readonly string[] = [
  '', 'opened a table', 'dealt', 'attacked', 'covered', 'passed', 'picked up', 'bout closed', 'went out',
  'game over', 'rematch', 'table closed', 'move refused', 'hostile move', 'stalled', 'resynced',
];
export const HOSTILE_WORD: readonly string[] = ['', 'a card not in hand', 'bytes that are not a move', 'an attack of 28 cards', 'a move at another table'];
export const PHASE_WORD: readonly string[] = ['lobby', 'playing', 'over', 'closed'];

// ---- formatting ------------------------------------------------------------------------------
export function int(n: number): string {
  return n.toLocaleString();
}

export function usToText(us: number): string {
  if (us < 1000) return `${Math.round(us)} µs`;
  return `${(us / 1000).toFixed(us < 10000 ? 2 : 1)} ms`;
}

export function uptime(s: number): string {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m ${s % 60}s`;
}

export function ago(ms: number, now: number): string {
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 5) return 'now';
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m`;
  return `${Math.floor(m / 60)}h`;
}

export function secondsText(s: number): string {
  if (s < 60) return `${s}s`;
  return `${Math.floor(s / 60)}m ${s % 60}s`;
}

export function mb(n: number): string {
  return `${n}`;
}
