// kernel.ts - the browser's kernel (c/wasm/web.c, built to /chess.wasm): loads it, checks its
// layout against the generated readers, and names its exports. Every rule and every protocol byte
// is in there; this file only moves bytes in and out of its io buffer.
import { memOf, readCvArena, readCvArenaList, readCvChat, readCvChatErr, readCvEval, readCvGames, readCvLobby, readCvMoves, readCvProfile, readCvPuzzle, readCvTop, readCvTv, readCvView } from '@/gen/web_layout';
import type { CvArena_Snap, CvArenaList_Snap, CvChat_Snap, CvChatErr_Snap, CvEval_Snap, CvGames_Snap, CvLobby_Snap, CvMoves_Snap, CvProfile_Snap, CvPuzzle_Snap, CvTop_Snap, CvTv_Snap, CvView_Snap } from '@/gen/web_layout';
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
  cv_eval_ask(ply: number): void;
  cv_eval(): number;
  cv_guest_size(): number;
  cv_guest_auth_body(mode: number): number;
  cv_read(pathLen: number, tag: number): void;
  cv_read_take(): number;
  cv_top(n: number): number;
  cv_speed(base: number, inc: number): number;
  cv_puzzle_ask(tag: number): void;
  cv_puzzle_load(n: number): number;
  cv_puzzle(): number;
  cv_puzzle_begin(): number;
  cv_puzzle_try(from: number, to: number, promo: number): number;
  cv_puzzle_reply(): number;
  cv_puzzle_targets_lo(from: number): number;
  cv_puzzle_targets_hi(from: number): number;
  cv_puzzle_is_promotion(from: number, to: number): number;
  cv_puzzle_reveal(): number;
  cv_puzzle_send(win: number, tag: number): void;
  cv_puzzle_done(n: number): number;
  cv_arena(now: number): number;
  cv_arena_read(n: number, now: number): number;
  cv_arena_watch(id: number): void;
  cv_arena_unwatch(id: number): void;
  cv_arena_join(id: number, tag: number): void;
  cv_arena_withdraw(id: number, tag: number): void;
  cv_arena_name_ok(n: number): number;
  cv_arena_create(base: number, inc: number, rated: number, startIn: number, duration: number, n: number, tag: number): void;
  cv_arena_list(n: number): number;
  cv_chat(n: number): number;
  cv_chat_log(n: number): void;
  cv_chat_say(rn: number, tn: number, tag: number): void;
  cv_chat_check(n: number): number;
  cv_chat_refused(n: number): number;
}

const utf8 = (s: string): Uint8Array => new TextEncoder().encode(s);

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
    return squares(premove ? this.x.cv_premove_targets_lo(from) : this.x.cv_targets_lo(from),
                   premove ? this.x.cv_premove_targets_hi(from) : this.x.cv_targets_hi(from));
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
  /** Asks the engine about the position after `ply` moves of the game shown. */
  evalAsk(ply: number): void { this.x.cv_eval_ask(ply); }
  /** Its last answer, with its line in SAN. */
  evaluation(): CvEval_Snap { return readCvEval(memOf(this.x.memory.buffer), this.x.cv_eval()); }
  /** GET /c/top/<speed>'s answer, read. */
  top(body: Uint8Array): CvTop_Snap { return readCvTop(memOf(this.x.memory.buffer), this.x.cv_top(this.put(body))); }
  /** A time control's speed (speed.h CC_BULLET..): which rating a game of it moves. */
  speed(base: number, inc: number): number { return this.x.cv_speed(base, inc); }
  /** Puzzles: solved on the kernel's board (puzzle.h), handed out and rated by the server. */
  puzzleAsk(tag: number): void { this.x.cv_puzzle_ask(tag); }
  puzzleLoad(body: Uint8Array): CvPuzzle_Snap { return this.pz(this.x.cv_puzzle_load(this.put(body))); }
  puzzle(): CvPuzzle_Snap { return this.pz(this.x.cv_puzzle()); }
  /** The opponent's move that sets the puzzle, played after its position was shown. */
  puzzleBegin(): CvPuzzle_Snap { return this.pz(this.x.cv_puzzle_begin()); }
  puzzleTry(from: number, to: number, promo: number): CvPuzzle_Snap { this.x.cv_puzzle_try(from, to, promo); return this.puzzle(); }
  puzzleReply(): CvPuzzle_Snap { return this.pz(this.x.cv_puzzle_reply()); }
  puzzleTargets(from: number): Set<number> { return squares(this.x.cv_puzzle_targets_lo(from), this.x.cv_puzzle_targets_hi(from)); }
  puzzleIsPromotion(from: number, to: number): boolean { return this.x.cv_puzzle_is_promotion(from, to) !== 0; }
  /** The line's next move shown: false at its end. */
  puzzleReveal(): boolean { return this.x.cv_puzzle_reveal() !== 0; }
  puzzleSend(win: boolean, tag: number): void { this.x.cv_puzzle_send(win ? 1 : 0, tag); }
  puzzleDone(body: Uint8Array): CvPuzzle_Snap { return this.pz(this.x.cv_puzzle_done(this.put(body))); }
  private pz(p: number): CvPuzzle_Snap { return readCvPuzzle(memOf(this.x.memory.buffer), p); }
  /** The arena shown (its room's last push or the last answer taken), its countdowns as of now. */
  arena(now: number): CvArena_Snap { return readCvArena(memOf(this.x.memory.buffer), this.x.cv_arena(now)); }
  /** The answer of GET /c/arena/<id>, a join or a withdrawal, taken as the arena shown. */
  arenaRead(body: Uint8Array, now: number): CvArena_Snap { return readCvArena(memOf(this.x.memory.buffer), this.x.cv_arena_read(this.put(body), now)); }
  arenaWatch(id: number): void { this.x.cv_arena_watch(id); }
  arenaUnwatch(id: number): void { this.x.cv_arena_unwatch(id); }
  arenaJoin(id: number, tag: number): void { this.x.cv_arena_join(id, tag); }
  arenaWithdraw(id: number, tag: number): void { this.x.cv_arena_withdraw(id, tag); }
  /** Whether an arena may have this name (the kernel's rule, arena.h). */
  arenaNameOk(name: string): boolean {
    const b = new TextEncoder().encode(name);
    return this.x.cv_arena_name_ok(this.put(b)) !== 0;
  }
  arenaCreate(base: number, inc: number, rated: boolean, startIn: number, duration: number, name: string, tag: number): void {
    const b = new TextEncoder().encode(name);
    this.x.cv_arena_create(base, inc, rated ? 1 : 0, startIn, duration, this.put(b), tag);
  }
  /** GET /c/arenas's answer, read. */
  arenaList(body: Uint8Array): CvArenaList_Snap { return readCvArenaList(memOf(this.x.memory.buffer), this.x.cv_arena_list(this.put(body))); }
  /** A room's chat as the kernel keeps it ("g<id>", "a<id>", "lobby"), oldest first. */
  chat(room: string): CvChat_Snap { return readCvChat(memOf(this.x.memory.buffer), this.x.cv_chat(this.put(utf8(room)))); }
  /** Reads a room's last lines (CC_EV_CHAT when they arrive). */
  chatLog(room: string): void { this.x.cv_chat_log(this.put(utf8(room))); }
  /** Says a line in a room: answered as a read with this tag (then chatRefused for a refusal). */
  chatSay(room: string, text: string, tag: number): void {
    const r = utf8(room), t = utf8(text);
    const both = new Uint8Array(r.length + t.length);
    both.set(r, 0);
    both.set(t, r.length);
    this.put(both);
    this.x.cv_chat_say(r.length, t.length, tag);
  }
  /** 0 when the server would take this line's text, else why not (CC_CHAT_.., CV_CHAT_BANNED). */
  chatCheck(text: string): number { return this.x.cv_chat_check(this.put(utf8(text))); }
  /** A refusal of a line, read. */
  chatRefused(body: Uint8Array): CvChatErr_Snap { return readCvChatErr(memOf(this.x.memory.buffer), this.x.cv_chat_refused(this.put(body))); }
  /** GET /c/games/<name>'s answer, read. */
  games(body: Uint8Array): CvGames_Snap { return readCvGames(memOf(this.x.memory.buffer), this.x.cv_games(this.put(body))); }
}

/** A bitboard's two halves as a set of squares. */
function squares(lo: number, hi: number): Set<number> {
  const s = new Set<number>();
  for (let i = 0; i < 32; i++) {
    if ((lo >>> 0) & (1 << i)) s.add(i);
    if ((hi >>> 0) & (1 << i)) s.add(32 + i);
  }
  return s;
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
