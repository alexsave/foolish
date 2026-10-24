// kernel.ts - the browser's kernel (c/wasm/web.c, built to /chess.wasm): loads it, checks its
// layout against the generated readers, and names its exports. Every rule and every protocol byte
// is in there; this file only moves bytes in and out of its io buffer.
import { memOf, readCvGames, readCvLobby, readCvMoves, readCvProfile, readCvTop, readCvTv, readCvView } from '@/gen/web_layout';
import type { CvGames_Snap, CvLobby_Snap, CvMoves_Snap, CvProfile_Snap, CvTop_Snap, CvTv_Snap, CvView_Snap } from '@/gen/web_layout';
import { LAYOUT_HASH } from '@/gen/web_layout_hash';

interface Exports {
  memory: WebAssembly.Memory;
  cv_layout_hash(): number;
  io_ptr(): number;
  io_cap(): number;
  cv_init(): void;
  cv_auth_body(n1: number, n2: number, mode: number): number;
  cv_refresh_body(): number;
  cv_auth_answer(status: number, n: number): void;
  cv_hello(): void;
  cv_in(n: number, now: number): void;
  cv_out(): number;
  cv_seek(base: number, inc: number, rated: number): void;
  cv_unseek(): void;
  cv_challenge(base: number, inc: number, rated: number): void;
  cv_accept(id: number): void;
  cv_ai(base: number, inc: number, level: number, color: number): void;
  cv_lobby(): void;
  cv_lobby_leave(): void;
  cv_open(game: number): void;
  cv_close(): void;
  cv_move(from: number, to: number, promo: number, now: number): number;
  cv_act(act: number): void;
  cv_targets_lo(from: number): number;
  cv_targets_hi(from: number): number;
  cv_is_promotion(from: number, to: number): number;
  cv_premove(from: number, to: number, promo: number): number;
  cv_premove_cancel(): void;
  cv_premove_targets_lo(from: number): number;
  cv_premove_targets_hi(from: number): number;
  cv_view(now: number): number;
  cv_moves(): number;
  cv_lobby_view(): number;
  cv_board_at(ply: number): number;
  cv_profile(n: number): number;
  cv_games(n: number): number;
  cv_tv(now: number): number;
  cv_guest_new(): number;
  cv_guest_size(): number;
  cv_guest_auth_body(mode: number): number;
  cv_read(pathLen: number, tag: number): void;
  cv_read_take(): number;
  cv_top(n: number): number;
}

export class Kernel {
  constructor(private readonly x: Exports) {}

  private bytes(): Uint8Array { return new Uint8Array(this.x.memory.buffer); }
  private io(): number { return this.x.io_ptr(); }

  /** Copies b into the io buffer. */
  put(b: Uint8Array): number {
    const n = Math.min(b.length, this.x.io_cap());
    this.bytes().set(b.subarray(0, n), this.io());
    return n;
  }
  /** The io buffer's first n bytes, copied out. */
  take(n: number): Uint8Array { return this.bytes().slice(this.io(), this.io() + n); }

  authBody(name: string, password: string, mode: number): Uint8Array {
    const e = new TextEncoder();
    const a = e.encode(name), b = e.encode(password);
    const both = new Uint8Array(a.length + b.length);
    both.set(a, 0);
    both.set(b, a.length);
    this.put(both);
    return this.take(this.x.cv_auth_body(a.length, b.length, mode));
  }
  refreshBody(): Uint8Array { return this.take(this.x.cv_refresh_body()); }
  /** A new guest account record (opaque bytes the page keeps), from fresh random bytes. */
  guestNew(): Uint8Array {
    const r = new Uint8Array(32);
    crypto.getRandomValues(r);
    this.put(r);
    return this.take(this.x.cv_guest_new());
  }
  /** Whether kept bytes are a guest record of this kernel's layout. */
  isGuest(g: Uint8Array): boolean { return g.length === this.x.cv_guest_size(); }
  /** The POST /c/auth body for a kept guest record. */
  guestAuthBody(guest: Uint8Array, mode: number): Uint8Array {
    this.put(guest);
    return this.take(this.x.cv_guest_auth_body(mode));
  }
  /** A GET over the socket; its answer comes back as a CC_EV_READ with this tag. */
  read(path: string, tag: number): void {
    const p = new TextEncoder().encode(path);
    this.put(p);
    this.x.cv_read(p.length, tag);
  }
  readTake(): Uint8Array { return this.take(this.x.cv_read_take()); }
  authAnswer(status: number, body: Uint8Array): void { this.x.cv_auth_answer(status, this.put(body)); }
  hello(): void { this.x.cv_hello(); }
  in(frame: Uint8Array, now: number): void { this.x.cv_in(this.put(frame), now); }
  /** The frames waiting to be sent. */
  out(): Uint8Array[] {
    const frames: Uint8Array[] = [];
    for (let n = this.x.cv_out(); n > 0; n = this.x.cv_out()) frames.push(this.take(n));
    return frames;
  }

  seek(base: number, inc: number, rated: boolean): void { this.x.cv_seek(base, inc, rated ? 1 : 0); }
  unseek(): void { this.x.cv_unseek(); }
  challenge(base: number, inc: number, rated: boolean): void { this.x.cv_challenge(base, inc, rated ? 1 : 0); }
  accept(id: number): void { this.x.cv_accept(id); }
  ai(base: number, inc: number, level: number, color: number): void { this.x.cv_ai(base, inc, level, color); }
  lobby(): void { this.x.cv_lobby(); }
  lobbyLeave(): void { this.x.cv_lobby_leave(); }
  open(game: number): void { this.x.cv_open(game); }
  close(): void { this.x.cv_close(); }
  move(from: number, to: number, promo: number, now: number): number { return this.x.cv_move(from, to, promo, now); }
  act(a: number): void { this.x.cv_act(a); }
  premove(from: number, to: number, promo: number): number { return this.x.cv_premove(from, to, promo); }
  premoveCancel(): void { this.x.cv_premove_cancel(); }
  /** The squares the piece on `from` may move to (now, or as a premove while the other side thinks). */
  targets(from: number, premove = false): Set<number> {
    const s = new Set<number>();
    const lo = (premove ? this.x.cv_premove_targets_lo(from) : this.x.cv_targets_lo(from)) >>> 0;
    const hi = (premove ? this.x.cv_premove_targets_hi(from) : this.x.cv_targets_hi(from)) >>> 0;
    for (let i = 0; i < 32; i++) {
      if (lo & (1 << i)) s.add(i);
      if (hi & (1 << i)) s.add(32 + i);
    }
    return s;
  }
  isPromotion(from: number, to: number): boolean { return this.x.cv_is_promotion(from, to) !== 0; }
  view(now: number): CvView_Snap { return readCvView(memOf(this.x.memory.buffer), this.x.cv_view(now)); }
  moves(): CvMoves_Snap { return readCvMoves(memOf(this.x.memory.buffer), this.x.cv_moves()); }
  lobbyView(): CvLobby_Snap { return readCvLobby(memOf(this.x.memory.buffer), this.x.cv_lobby_view()); }
  /** The board (and last move) at an earlier ply. */
  boardAt(ply: number): CvView_Snap { return readCvView(memOf(this.x.memory.buffer), this.x.cv_board_at(ply)); }
  /** GET /c/user/<name>'s answer, read. */
  profile(body: Uint8Array): CvProfile_Snap { return readCvProfile(memOf(this.x.memory.buffer), this.x.cv_profile(this.put(body))); }
  /** The lobby's live games, their clocks as of now. */
  tv(now: number): CvTv_Snap { return readCvTv(memOf(this.x.memory.buffer), this.x.cv_tv(now)); }
  /** GET /c/top's answer, read. */
  top(body: Uint8Array): CvTop_Snap { return readCvTop(memOf(this.x.memory.buffer), this.x.cv_top(this.put(body))); }
  /** GET /c/games/<name>'s answer, read. */
  games(body: Uint8Array): CvGames_Snap { return readCvGames(memOf(this.x.memory.buffer), this.x.cv_games(this.put(body))); }
}

let loading: Promise<Kernel> | null = null;

export function loadKernel(): Promise<Kernel> {
  if (!loading) {
    loading = (async () => {
      const res = await fetch('/chess.wasm');   // matches the layout's preload (cors, same-origin credentials)
      const { instance } = await WebAssembly.instantiateStreaming(res, {});
      const x = instance.exports as unknown as Exports;
      if ((x.cv_layout_hash() >>> 0) !== (LAYOUT_HASH >>> 0)) throw new Error('chess.wasm and the page were built from different headers');
      x.cv_init();
      return new Kernel(x);
    })();
  }
  return loading;
}
