// TypeScript bridge to bots.wasm, the kernel every host runs (c/src plus every
// bot strategy and the wasm entries in c/wasm/).
//
// The game itself is never marshalled from here: servers and tests hold a game
// as the C Table (sdk/ts/table/server_table.ts) and the web as its client slot
// (sdk/ts/table/client_table.ts), both over this module's instance. What is left
// here are the doors that take a handful of bytes rather than a board: the bot
// roster, the replay codec and its extras and links, the iMessage envelope and
// the animation rules. Loaded lazily and cached.

import { kernelModule, loadWasmGz, loadWasmGzAsync, type WasmModuleName } from './wasm_asset.ts';
import { LAYOUT_HASH as BOTS_LAYOUT_HASH } from '../gen/layout_hash.bots.ts';
import { assertLayoutHash } from './layout_hash.ts';
import type { BotsExports } from '../gen/exports.bots.ts';
import type { WebExports } from '../gen/exports.web.ts';
import * as V from '../gen/view_layout.bots.ts';
import * as M from '../gen/msg_layout.bots.ts';
import * as A from '../gen/anim.bots.ts';
import * as K from '../gen/kernel_consts.bots.ts';
import { MsgErrorText } from '../gen/errtext/msg.ts';
import { ReplayErrorText } from '../gen/errtext/replay.ts';
import { ReplayExtrasErrorText } from '../gen/errtext/replay_extras.ts';

/** A card as the kernel's doors here read and write it (suit 0..3, value 1..13; -1/-1 is a hidden card). */
export type Card = V.Card_Snap;

let exportsCache: WebExports | null = null;
let linked: WasmModuleName = 'web';

/**
 * Prepare the kernel where it cannot be read synchronously - i.e. the browser,
 * which has no filesystem and must FETCH the .gz. Await this once before any
 * bots() call; on the server it is a no-op fast path (fs is synchronous).
 *
 * WHICH module: kernelModule() answers, and in the browser the answer is
 * web.wasm - the same objects as bots.wasm under a smaller export allow-list,
 * so the browser gets the client slot, the animation plan, the replay reader
 * and the FMSG decode it calls, and none of the C Table or the Monte-Carlo
 * brains it does not. One source, one layout hash, two links (c/Makefile,
 * WASM_WEB_NAMES; the reach set is pinned by e2e/wasm_web_link.test.ts).
 *
 * Deliberately a fetched ASSET rather than a base64 twin of the same bytes: a
 * second carrier of one kernel goes stale while the other is rebuilt, and 80 KB
 * of single-line base64, rewritten on every kernel build, is miserable in git.
 * The cost is that the browser's door is async; callers await this first.
 */
export async function ensureBotsAsync(): Promise<void> {
    if (exportsCache) return;
    await loadWasmGzAsync(kernelModule());   // caches the inflated bytes for bots()
    bots();
}

/** The warm module's exports, for the web client's slot (sdk/ts/table/client_table.ts), which types what it calls. */
export function __clientKernelExports(): WebAssembly.Exports {
    return bots() as unknown as WebAssembly.Exports;
}

// The kernel as EVERY link exports it: web.wasm's generated interface, which
// bots.wasm's contains (the browser's exports are picked out of the server's,
// c/Makefile WASM_WEB_EXPORTS). A door typed against this runs in a browser.
function bots(): WebExports {
    if (exportsCache) return exportsCache;
    linked = kernelModule();
    const module = new WebAssembly.Module(loadWasmGz(linked) as BufferSource);
    const ex = new WebAssembly.Instance(module, {}).exports as unknown as WebExports;
    // ONE hash for both links, because they are one compile: the constant is
    // generated from the bots build's flags and web.wasm is those same objects.
    // A mismatch here means the two came from different trees, which is the one
    // way a per-call-site link could ever become a second kernel.
    assertLayoutHash(`${linked}.wasm`, ex, BOTS_LAYOUT_HASH, 'sdk/ts/gen/layout_hash.bots.ts');
    ex.wasm_init();
    // THE TRANSPORT, said once (anim_plan.h). Every host that reaches the
    // kernel through this module is the server shape: a card's confirmation is
    // its own later broadcast, so "the newest news does not mention my card"
    // means the receipt is in the post, not that it was rejected. The kernel
    // has no default for this and errors rather than guessing, which is what
    // keeps a new host from silently inheriting iMessage's answer.
    ex.wasm_anim_set_transport(K.ANIM_TRANSPORT_SERVER);
    exportsCache = ex;
    return ex;
}

// The kernel as bots.wasm alone exports it, for the doors the browser's link
// leaves out (reading a pasted link back, which only the server does). Where
// web.wasm is the module loaded, such a door is refused here, by name, instead
// of failing as "x is not a function".
function serverBots(): BotsExports {
    const ex = bots();
    if (linked !== 'bots') throw new Error(`this kernel door is exported by bots.wasm only, and ${linked}.wasm is loaded`);
    return ex as BotsExports;
}

// One cached view over the whole linear memory, in the shape every generated
// reader takes. The kernel never mallocs, so the buffer identity is stable, but
// a detached buffer is re-viewed anyway.
let memView = V.memOf(new ArrayBuffer(0));
function mem(ex: WebExports): V.Mem {
    if (memView.u8.buffer !== ex.memory.buffer) memView = V.memOf(ex.memory.buffer);
    return memView;
}

/** Byte strings back to back in the kernel's replay-io buffer, where every call
 *  here reads its input; `tooBig` is thrown when they do not fit. */
function replayIn<E extends WebExports>(ex: E, tooBig: () => Error, ...parts: Uint8Array[]): E {
    let at = ex.wasm_replay_io_ptr();
    const end = at + ex.wasm_replay_io_cap();
    for (const part of parts) {
        if (part.length > end - at) throw tooBig();
        mem(ex).u8.set(part, at);
        at += part.length;
    }
    return ex;
}

/** A kernel answer that is not a refusal: a negative one is thrown as its family's sentence. */
function ok(rc: number, refusal: (code: number) => Error): number {
    if (rc < 0) throw refusal(rc);
    return rc;
}

/** `n` bytes of an answer, copied out of the kernel's io buffer or from `at`. */
function taken(ex: WebExports, n: number, at = ex.wasm_io_ptr()): Uint8Array {
    return mem(ex).u8.slice(at, at + n);
}

const utf8 = (text: string) => new TextEncoder().encode(text);
const str = (bytes: Uint8Array) => new TextDecoder().decode(bytes);
// The kernel terminates a string in place, one byte past it, so a string goes
// in with that byte behind it and is refused unless both fit.
const NUL = new Uint8Array(1);

// What the kernel's refusal codes say, from its own table (c/errtext/error_text.h,
// written out by tools/datagen); only the sentence for a code that table does
// not hold is this file's.
const refusalText = (rows: readonly { code: number; text: string }[], code: number, family: string): string =>
    rows.find((row) => row.code === code)?.text ?? `${family} kernel error ${code}`;
const msgError = (code: number) => new Error(refusalText(MsgErrorText, code, 'iMessage payload:'));
const extrasError = (code: number) => new Error(refusalText(ReplayExtrasErrorText, code, 'extras:'));
const animError = (entry: string) => (code: number) => new Error(`${entry} error ${code}`);
const msgTooBig = () => msgError(M.MSG_ECAP);
const extrasTooBig = () => extrasError(-M.REPLAY_EXTRAS_ECAP);
const codeTooBig = () => replayError(-K.REPLAY_ECAP);
const tooBig = (what: string) => () => new Error(`${what}: does not fit the kernel IO buffer`);

/** A replay refusal as a sentence. `d` is its parameters (replay.h ReplayError),
 *  which fill the table's {version}, {log_type} and {menu}; zeros stand in when
 *  a caller has only the code. */
function replayError(negCode: number, d: V.ReplayError_Snap = { version: 0, logType: 0, menu: 0 }): Error {
    return new Error(refusalText(ReplayErrorText, negCode, 'replay')
        .replace('{version}', `${d.version}`).replace('{menu}', `${d.menu}`)
        .replace('{log_type}', K.LOG_NAMES[d.logType]?.toLowerCase() ?? `${d.logType}`));
}

/** The module's last replay refusal, with the parameters the kernel kept for it. */
function replayRefused(negCode: number): Error {
    const ex = bots();
    return replayError(negCode, V.readReplayError(mem(ex), ex.wasm_replay_error_ptr()));
}

/** A replay code into the kernel, for the wasm_replay_* call that reads it. */
const codeIn = (code: Uint8Array) => replayIn(bots(), codeTooBig, code);

/** One row of the kernel's bot roster (c/src/bot_roster.h BotRosterRow). */
export type BotRosterEntry = V.BotRosterRow_Snap;

let rosterCache: readonly BotRosterEntry[] | null = null;

/** The kernel's bot roster (bot_roster.h): hosts look it up, never restate it. */
export function kernelBotRoster(): readonly BotRosterEntry[] {
    if (rosterCache) return rosterCache;
    const ex = bots();
    const at = ex.wasm_bot_roster_dump();
    if (!at) throw new Error('bot_roster: the kernel refused to dump the table');
    return rosterCache = V.readBotRosterTable(mem(ex), at).rows;
}

// ---------- FMSG: the iMessage envelope (c/src/msg_wire.h) -------------
//
// An iMessage game has no server: the whole game is (32-byte deal seed, v6
// replay code) in an MSMessage URL, and every device rebuilds it by re-dealing
// from the seed and replaying the code through the kernel. The web only READS
// one - a shared link's unfurl and its page - so this is the one door: the
// envelope's layout lives in C, once, so the phone and the web can never read
// the same bytes as different games. Sealing, rebasing and Rule P are the
// phone's (c/ios/ios_api_msg.c) and are tested beside the kernel
// (c/tests/msg_wire_test.c).
//
// It rides the REPLAY io buffer, never g_io: an FMSG call IS a replay call (its
// body is a v6 code), so an envelope in g_io would be clobbered by the codec's
// own bignum scratch mid-decode. See wasm_api.c.

// Decode + VALIDATE an envelope: the chain is replayed through the kernel, so a
// corrupt or hand-edited payload throws rather than half-loading (§7.3 -
// validation IS replay).
//
// The header comes back as the C struct itself (msg_wire.h MsgHeader, at
// wasm_msg_header_ptr), read by its generated reader: the envelope is `.e` and
// the SHA-256 of its bytes is `.digest`.
export function kernelMsgDecode(envelope: Uint8Array): M.MsgHeader_Snap {
    const ex = replayIn(bots(), msgTooBig, envelope);
    ok(ex.wasm_msg_decode(envelope.length), msgError);
    return M.readMsgHeader(mem(ex), ex.wasm_msg_header_ptr());
}

/* ---------------- the replay code's extras blob (c/src/replay_extras.h) ------
 *
 * The nicknames and per-move timing behind the dash in a share link. The codec
 * is the kernel's, and what a host does with it here is READ a blob and build a
 * link around a roster; a finished game's blob is written by the C Table
 * (sdk/ts/table/server_table.ts). Synchronous, assuming a warm module, like
 * every other reader here. Callers catch an extras failure and fall back to
 * "P1"/"P2".
 */

/** What a decoded extras blob says. */
interface KernelReplayExtras {
    names: string[] | null;
    startTime: number | null;
    moveGaps: number[] | null;
}

/**
 * The raw extras blob -> roster + timing. `playerCount` and `moveCount` come
 * from the DECODED MOVES: the blob carries neither, and reading it needs both.
 * Throws on a malformed blob.
 */
export function kernelReplayExtrasDecode(blob: Uint8Array, playerCount: number,
                                         moveCount: number): KernelReplayExtras {
    const ex = replayIn(bots(), extrasTooBig, blob);
    ok(ex.wasm_replay_extras_decode(blob.length, playerCount, moveCount), extrasError);
    const x = M.readReplayExtras(mem(ex), ex.wasm_replay_extras_ptr());
    const hasTimes = (x.flags & M.REPLAY_EXTRAS_FLAG_TIMES) !== 0;
    return {
        names: (x.flags & M.REPLAY_EXTRAS_FLAG_NAMES) ? x.names.map((n) => n.text) : null,
        startTime: hasTimes ? x.startTime : null,
        moveGaps: hasTimes ? [...x.gaps] : null,
    };
}

// base32, from the kernel (replay.c replay_b32_encode/decode). replay.h called
// this alphabet "the web's codec.ts alphabet" and replay.c said a code made on
// the web "reads here byte for byte" - a mirror documenting itself as one. The
// web asks for it now, so there is one alphabet.
export function kernelB32Encode(bytes: Uint8Array): string {
    const ex = replayIn(bots(), tooBig('b32'), bytes);
    return str(taken(ex, ok(ex.wasm_replay_b32_encode(bytes.length), tooBig('b32'))));
}

// Accepts lower case, ignores characters outside the alphabet, and stops at the
// '-' where a share link's extras suffix begins. All of that is the kernel's
// decoder, not a convention restated here.
export function kernelB32Decode(code: string): Uint8Array {
    const text = utf8(code);
    const ex = replayIn(bots(), tooBig('b32'), text, NUL);
    return taken(ex, ok(ex.wasm_replay_b32_decode(text.length), tooBig('b32')));
}

// The replay code out of whatever a person pasted - the kernel's
// replay_link_parse. Building a link and reading one back are two halves of the
// same format, and this is the half that has to REFUSE: replay_b32_decode is
// deliberately tolerant, so an unstripped "https" prefix would decode to a
// different game rather than fail. Throws when the input is not a code.
export function kernelReplayLinkParse(url: string): string {
    const text = utf8(url);
    const ex = replayIn(serverBots(), tooBig('link'), text, NUL);
    return str(taken(ex, ok(ex.wasm_replay_link_parse(text.length), () => new Error(`not a replay code: "${url}"`))));
}

/**
 * THE WHOLE SHAREABLE LINK for a finished game: `https://foolish.cards/<moves>`
 * plus `-<base32 extras>` when the roster says anything, and the bare link when
 * nobody at the table is named. Built in the kernel rather than concatenated
 * here (c/src/replay_extras.h) - the prefix is a constant and the dash is the
 * codec's, so a phone, a watch and a browser have no business each writing it.
 * `names` must be as wide as the table; unnamed seats are ''. `style` is a
 * REPLAY_LINK_STYLE_*: the https link, or the QR form.
 *
 * The roster goes through the kernel's own struct; the moves code crosses as
 * itself, since a long game's code runs to tens of KB.
 */
export function kernelReplayLink(moves: string, names: string[], style: number = M.REPLAY_LINK_STYLE_URL): string {
    const code = utf8(moves);
    const ex = replayIn(bots(), extrasTooBig, code, NUL);
    // The roster, through the generated writer: a name too wide for the
    // struct's slot is its RangeError.
    M.writeReplayExtras(mem(ex), ex.wasm_replay_extras_ptr(), {
        flags: names.length > 0 ? M.REPLAY_EXTRAS_FLAG_NAMES : 0,
        names: names.map((text) => ({ text })), startTime: 0, gaps: [],
    });
    return str(taken(ex, ok(ex.wasm_replay_link(code.length, style), extrasError)));
}

// ---------------------------------------------------------------------------
// Replay steps (docs/C_CORE_CONSOLIDATION.md F4.2 / A5)
//
// A v6 code, rebuilt into the real Game and replayed through the real engine,
// handed back as the SAME packed evwire frames live play broadcasts. The caller
// reads them with the client slot's push reader - the one it uses for live play -
// so a replay is not a second rendering path.
// ---------------------------------------------------------------------------

/** Steps a code replays to: the deal, then one per action. */
function replayStepCount(code: Uint8Array): number {
    return ok(codeIn(code).wasm_replay_step_count(code.length), replayRefused);
}

/** One step as a replay screen plays it (replay_steps.h ReplayPlayStep): what it
 *  is, whether it `joins` the step before it into one move, whether it opens a
 *  bout, and when it happened (`at`, unix seconds). */
export type ReplayPlayStep = V.ReplayPlayStep_Snap;

/**
 * Every step of a code as a replay screen plays it, against the extras' clock:
 * `gaps` are the per-move gaps and `startTime` the first move's time, null for a
 * code cut without them (nothing joins, and `at` says nothing).
 */
export function replayPlaySteps(code: Uint8Array, startTime: number | null, gaps: number[] | null): ReplayPlayStep[] {
    const ex = codeIn(code);
    M.writeReplayExtras(mem(ex), ex.wasm_replay_extras_ptr(), {
        flags: gaps ? M.REPLAY_EXTRAS_FLAG_TIMES : 0,
        names: [], startTime: gaps ? startTime ?? 0 : 0, gaps: gaps ?? [],
    });
    const n = ok(ex.wasm_replay_step_play(code.length), replayRefused);
    const m = mem(ex), base = ex.wasm_io_ptr();
    return Array.from({ length: n }, (_, i) => V.readReplayPlayStep(m, base + i * K.RS_PLAY_STRIDE));
}

/**
 * Every evwire frame a code replays to, for `viewer` (-1 = spectator), in step
 * order. Pulled in chunks because the frames of a whole game - each carrying a
 * masked board snapshot - outgrow the wasm IO buffer, and because evwire's
 * n_events is a u8, so one frame per game is impossible regardless.
 */
export function replayEventFrames(code: Uint8Array, viewer: number): Uint8Array[] {
    const frames: Uint8Array[] = [];
    const steps = replayStepCount(code);
    let from = 0;
    // Each chunk re-runs the replay from the start (the arithmetic decode is the
    // cost and it is ~1ms); a game takes a couple of chunks. The guard is a
    // no-progress backstop, not a step limit. Where each frame lies in the chunk
    // is the kernel's own index (replay_steps.h ReplayFrameIndex); the frames
    // themselves are opaque bytes forwarded to the same decoder live play feeds.
    for (let guard = 0; from < steps && guard < 4096; guard++) {
        const ex = codeIn(code);
        ok(ex.wasm_replay_events(viewer, from, code.length), replayRefused);
        const index = V.readReplayFrameIndex(mem(ex), ex.wasm_replay_events_index_ptr());
        if (index.nextStep <= from) throw new Error(`replay frames stalled at step ${from}/${steps}`);
        const base = ex.wasm_io_ptr();
        index.off.forEach((off, i) => frames.push(taken(ex, index.len[i], base + off)));
        from = index.nextStep;
    }
    if (frames.length !== steps) {
        throw new Error(`replay produced ${frames.length} frames for ${steps} steps`);
    }
    return frames;
}

/** What a code says about its game as a whole (c/src/replay_steps.h ReplaySummary). */
export type ReplaySummary = V.ReplaySummary_Snap;

/**
 * A code at a glance, without playing it back: the seat count, the trump suit and
 * the opener, the fool (-1 for a code cut mid-game) and the order the others went
 * out, and how many moves its extras time. null when the code does not decode.
 */
export function replaySummary(code: Uint8Array): ReplaySummary | null {
    const ex = codeIn(code);
    const at = ex.wasm_replay_summary(code.length);
    return at > 0 ? V.readReplaySummary(mem(ex), at) : null;
}

/**
 * The board action step `step` of a code was decided on, as `viewer` saw it: the
 * bytes a masked wasm_import_state reads, for the Oracle to import unchanged
 * (c/src/replay_steps.h replay_steps_board_v6). null when the step is not an
 * action or the viewer is not a seat.
 */
export function replayStepMaskedState(code: Uint8Array, step: number, viewer: number): Uint8Array | null {
    const ex = codeIn(code);
    const len = ex.wasm_replay_step_masked_state(code.length, step, viewer);
    return len < 0 ? null : taken(ex, len);
}

/**
 * The public log before action step `step`'s move, as the Oracle's memory: the
 * bytes wasm_import_logs reads (c/src/replay_steps.h replay_steps_memory_v6).
 * null when the step has no record of its own to pair with (a good, a round end).
 */
export function replayStepLogs(code: Uint8Array, step: number): Uint8Array | null {
    const ex = codeIn(code);
    const len = ex.wasm_replay_step_logs(code.length, step);
    return len < 0 ? null : taken(ex, len);
}

// ===========================================================================
// Animation core (c/src/anim_plan.h)
// ===========================================================================
// The bridge the web pure modules (src/state/*) delegate to, so the animation
// policy the React glitch-fixing hardened lives in C once - the same "one kernel
// behind every host" argument FMSG makes. The kernel is loaded at app boot
// (providers.tsx awaits ensureBotsAsync), and bots() is synchronous on the
// server, so these calls are safe synchronously in both.

// The pipeline's event-type and location NAMES against the kernel's codes: the
// wire the web decodes from (src/state/pushSequence.ts) speaks strings, and they
// are the kernel's own ANIM_EVT_* and ANIM_LOC_* names in lower case, turned
// round out of the generated value -> name tables.
const codesByName = (names: Readonly<Record<number, string>>): Record<string, number> =>
    Object.fromEntries(Object.entries(names).map(([code, name]) => [name.toLowerCase(), Number(code)]));
export const ANIM_EVT = codesByName(K.ANIM_EVT_NAMES);
export const ANIM_LOC = codesByName(K.ANIM_LOC_NAMES);

/** The event-type string -> ANIM_EVT_* code (0 for an unknown/None type). */
export function animEventTypeCode(type: string | undefined): number {
    return (type && type in ANIM_EVT) ? ANIM_EVT[type] : 0;
}

/** The live feed's version gate (anim_plan.h anim_should_drop_stale). null models "no version"
 *  (a replay sequence, never gated). */
export function animShouldDropStale(last: number | null, incoming: number | null): boolean {
    return bots().wasm_anim_should_drop_stale(
        last === null ? 0 : 1, last ?? 0,
        incoming === null ? 0 : 1, incoming ?? 0) !== 0;
}

/** anim_plan.h anim_stale_optimistic_ask (the browser asks it inside client_push_settle). Returns the INDICES
 *  into `optCards` to release. */
export function animStaleOptimisticOnTable(optCards: Card[], tableCards: Card[], namedCards: Card[]): number[] {
    const ex = serverBots();
    V.writeAnimStaleAsk(mem(ex), ex.wasm_anim_stale_ask_ptr(), { opt: optCards, table: tableCards, named: namedCards });
    ok(ex.wasm_anim_stale_optimistic(), animError('anim_stale_optimistic'));
    return [...V.readAnimStaleRelease(mem(ex), ex.wasm_anim_stale_release_ptr()).release];
}

// ---- the role beat (anim_plan.h "the role beat") -----------------------------
//
// WHICH MARKS CHANGE AT WHICH POINT OF A SEQUENCE, which is three timings and
// not one: a good being SET leads the stream, a good being CLEARED runs with the
// throw-in that cleared it, a PASS hands the shield over with the transfer card,
// and everything else waits for the closing beat. The rules are the kernel's
// (anim_goods_opening / anim_goods_cleared / anim_pass_hand_off) and iOS has
// reached them since round 28; these four are the same C, reached from a
// browser.
//
// `shown` is WHAT THE BADGES ARE WEARING and not the live board - the host's
// frozen ledger (src/state/roleLedger.ts), which is why it crosses as a value.

/** The three facts a role mark is drawn from (anim_plan.h AnimRoles). A seat is
 *  -1 for none, and `goodMask` is one bit per seat. */
export type AnimRoles = V.AnimRoles_Snap;

/** What every role entry answers: the marks to show next where the kernel left them, or null for "nothing changed". */
function rolesAnswer(ex: WebExports, at: number): AnimRoles | null {
    return ok(at, animError('anim roles')) === 0 ? null : V.readAnimRoles(mem(ex), at);
}

/** THE STATE A STREAM SHOULD OPEN ON (anim_goods_opening), or null for "start
 *  playing straight away". `firstGoodMask` is the stream's event-0 mask, or
 *  ANIM_NO_MASK for a step that carried no board. */
export function animRolesGoodsOpening(shown: AnimRoles, firstGoodMask: number): AnimRoles | null {
    const ex = bots();
    return rolesAnswer(ex, ex.wasm_anim_roles_goods_opening(
        shown.defender, shown.firstAttacker, shown.goodMask, firstGoodMask));
}

/** THE MIRROR IMAGE (anim_goods_cleared), played with the throw-in that cleared
 *  it. `stepGoodMask` is that beat's own mask. */
export function animRolesGoodsCleared(shown: AnimRoles, stepGoodMask: number): AnimRoles | null {
    const ex = bots();
    return rolesAnswer(ex, ex.wasm_anim_roles_goods_cleared(
        shown.defender, shown.firstAttacker, shown.goodMask, stepGoodMask));
}

/** A PASS (anim_pass_hand_off): the shield travels with the transfer card.
 *  `attackPassSeats` is the bit per seat that laid cards in this beat, and the
 *  kernel - not the wire - decides which of them was a transfer.
 *  `finalDefender` is the stream's FINAL board's defender, the only place a
 *  pass's new defender ever appears. */
export function animRolesPassHandOff(shown: AnimRoles, attackPassSeats: number,
                                     finalDefender: number): AnimRoles | null {
    const ex = bots();
    return rolesAnswer(ex, ex.wasm_anim_roles_pass_hand_off(
        shown.defender, shown.firstAttacker, shown.goodMask, attackPassSeats, finalDefender));
}

/** WHAT A CHANGE OF THE MARKS SOUNDS LIKE (anim_role_cue): the ANIM_CUE_* for
 *  the badges going from `shown` to `next`, ANIM_CUE_NONE for a change that
 *  makes no sound. Asked where the marks are turned, so no screen decides which
 *  turn is audible. */
export function animRoleCue(shown: AnimRoles, next: AnimRoles): number {
    return bots().wasm_anim_role_cue(
        shown.defender, shown.firstAttacker, shown.goodMask,
        next.defender, next.firstAttacker, next.goodMask);
}

/** WHAT THE END OF THE GAME SOUNDS LIKE to this viewer (anim_finish_cue).
 *  `gameOver` is the fool's seat, negative while the game runs; `mySeat` is
 *  negative for a spectator. */
export function animFinishCue(gameOver: number, mySeat: number): number {
    return bots().wasm_anim_finish_cue(gameOver, mySeat);
}

/** MAY THIS WRITER TOUCH THE SHOWN BADGES (anim_shown_ledger_allows)? `claim` is
 *  an ANIM_CLAIM_*: a bystander may not, while a sequence is running; everyone
 *  else may. */
export function animShownLedgerAllows(claim: number, sequencing: boolean): boolean {
    return bots().wasm_anim_shown_ledger_allows(claim, sequencing ? 1 : 0) !== 0;
}

/** One row of the end screen's finish order (anim_plan.h AnimFinishRow). */
interface AnimFinishRow { place: number; seat: number; isYou: boolean }

/** THE FINISH ORDER, in C (anim_plan.h anim_finish_rows). Rank 1 is the first
 *  seat out, counting up to the fool last - whose place is the SEAT count, not
 *  the row count. `elimination` is first-out first; `gameOver` is the fool's
 *  seat, or negative while the game is still running; `mySeat` negative for a
 *  spectator, who owns no row. */
export function animFinishRows(
    elimination: number[], gameOver: number, nPlayers: number, mySeat: number,
): AnimFinishRow[] {
    const ex = bots();
    V.writeAnimFinishAsk(mem(ex), ex.wasm_anim_finish_ask_ptr(), { gameOver, nPlayers, mySeat, elimination });
    ok(ex.wasm_anim_finish_rows(), animError('anim_finish_rows'));
    return V.readAnimFinish(mem(ex), ex.wasm_anim_finish_ptr()).rows
        .map((r) => ({ place: r.place, seat: r.seat, isYou: r.isYou !== 0 }));
}

// "No card" as the kernel holds it (card.h CARD_NONE), spelled from its
// generated fields. A hand slot the caller cannot name crosses as it (anim_plan.h
// AnimHandAsk reads it as a back on the way in and writes one on the way out).
const NO_CARD: Card = { suit: V.CARD_NONE_SUIT, value: V.CARD_NONE_VALUE };

/**
 * THE ORDER A HAND IS DRAWN IN, in C (anim_plan.h anim_hand_laid_out_masked).
 *
 * `cards` is the hand as the kernel hands it over and `order` the viewer's
 * preferred arrangement, both of them plain cards with `null` for a slot the
 * caller cannot name. Cards the order no longer holds drop out, cards it never
 * knew append in kernel order, and a face-down slot is reconciled by COUNT,
 * because it has no identity to be stale about.
 *
 * ONE DOOR. The web had three of these - mergeReplayHandOrder by count,
 * mergeHandOrder by key set, reconcileHandMemory/displayedHand by key - so one
 * rearrangement scrubbed through a replay and played live could come out two
 * different ways. Asserted natively (c/tests/anim_plan_test.c
 * test_hand_order_with_hidden_slots).
 */
export function animHandLaidOut(
    cards: readonly (Card | null)[], order: readonly (Card | null)[], deferred = 0n,
): (Card | null)[] {
    const ex = bots();
    V.writeAnimHandAsk(mem(ex), ex.wasm_anim_hand_ask_ptr(), {
        deferred, cards: cards.map((c) => c ?? NO_CARD), order: order.map((c) => c ?? NO_CARD),
    });
    ok(ex.wasm_anim_hand_laid_out(), animError('anim_hand_laid_out'));
    return V.readAnimHandLaid(mem(ex), ex.wasm_anim_hand_laid_ptr()).cards
        .map((c) => (c.suit === V.CARD_NONE_SUIT && c.value === V.CARD_NONE_VALUE ? null : c));
}

// ---------------------------------------------------------------------------
// The plan, the frame and the beats (anim_plan.h)
// ---------------------------------------------------------------------------
// WHAT A HOST WITH A FRAME LOOP ASKS. `animBuildPlan` turns a decoded viewer
// sequence into the kernel's timed plan - durations, start offsets, the
// count-freeze and the veil - and `animPlanAt` samples it at a clock the host
// owns, so a push landing mid-flight is answered by the next call instead of by
// editing the queue a timer chain is walking.
//
// The structs are read WHERE THEY LIE, through the generated accessors, and
// only the scalars a caller actually uses are copied out: a plan is 11,752
// bytes and a frame loop that marshalled it whole once per frame would spend
// more time copying than animating.

/** One step as the plan is asked it (client_table.h ClientPlanStep), and the board it left. */
export interface AnimPlanEventIn {
    type: number;                   // ANIM_EVT_*
    seat?: number;                  // the acting seat; absent for none
    from?: number; to?: number;     // ANIM_LOC_*
    cards?: readonly Card[];
    /** THIS step's own board, when it has one. */
    board?: V.TableView_Snap | null;
}

/** The count-freeze (anim_plan.h AnimCounts): the board the display holds until a step lands. */
export interface AnimCountsSnap {
    deck: number; discard: number; hand: number[]; nPlayers: number;
    nBattles: number; battles: number[]; paired: boolean; flipped: Card | null;
}

/** One planned step (anim_plan.h AnimPlanStep). `beatFirst`/`beatN` are the
 *  span of steps this one flies WITH - the kernel spends one COVER event per
 *  card, so a two-card cover is two steps and one beat - and `holdMs` is the
 *  rest the sequence takes after that beat lands, already inside the next
 *  beat's `startMs`. */
export interface AnimPlanStepSnap {
    type: number; seat: number; from: number; to: number; nCards: number;
    durationMs: number; startMs: number;
    beatFirst: number; beatN: number; holdMs: number;
    deck: number; discard: number; hand: number[];
    inFlightFromDeck: number; inFlightToFlipped: number;
    reveals: bigint;
    /** The flipped trump this step deals out, by its real identity for every
     *  viewer, or null; it is the last of the step's cards. */
    trumpOut: Card | null;
    /** Where that trump leaves from (ANIM_LOC_FLIPPED), ANIM_LOC_NONE without one. */
    trumpFrom: number;
}

/** One sound the plan makes (anim_plan.h AnimCue): `hits` strikes of `kind`
 *  (an ANIM_CUE_*), the first `atMs` from the sequence's start and each one
 *  after ANIM_SOUND_STAGGER_MS behind the last. `step` is the step that makes
 *  it, and (step, kind) names a cue uniquely within a plan. */
export interface AnimCueSnap {
    atMs: number; kind: number; step: number; hits: number; nCards: number;
}

/** The plan (anim_plan.h AnimPlan). */
export interface AnimPlanSnap {
    nSteps: number; totalMs: number; pre: AnimCountsSnap;
    veilIds: number[]; steps: AnimPlanStepSnap[];
    /** What the sequence sounds like, in step order (which is not time order). */
    cues: AnimCueSnap[];
}

/** Where the plan stands at a moment (anim_plan.h AnimFrame). `step` is
 *  ANIM_STEP_NONE when no step is playing at this instant, and `nextMs` is
 *  ANIM_NEVER when the answer will not change again. */
export interface AnimFrameSnap {
    step: number; elapsedMs: number; landed: number; nextMs: number; done: boolean;
    deck: number; discard: number; hand: number[]; nPlayers: number; flipped: Card | null;
    inFlightFromDeck: number; inFlightToFlipped: number;
    veiled: bigint;
    /** The trump in the air: the step in flight's `trumpOut`, or null. */
    trumpFlight: Card | null;
}

/** THE TIMED PLAN for a run of steps (client_table.h client_plan). `final` is
 *  the board the host already holds; the plan freezes the DISPLAY back to the
 *  pre-sequence values and reveals forward one step at a time. The kernel reads
 *  every count, the row and the masking off the boards themselves. */
export function animBuildPlan(events: readonly AnimPlanEventIn[], final: V.TableView_Snap | undefined): AnimPlanSnap {
    const ex = bots(), m = mem(ex), refused = animError('anim_build_plan');
    events.forEach((e, at) => {
        V.writeClientPlanStep(m, ex.wasm_client_plan_step_ptr(), {
            type: e.type,
            seat: e.seat === undefined || e.seat < 0 ? A.ANIM_SEAT_NONE : e.seat,
            from: e.from ?? A.ANIM_LOC_NONE, to: e.to ?? A.ANIM_LOC_NONE,
            cards: e.cards ?? [],
        });
        if (e.board) V.writeTableView(m, ex.wasm_client_edit_view_ptr(), e.board);
        ok(ex.wasm_client_plan_step(at, e.board ? 1 : 0), refused);
    });
    if (final) V.writeTableView(m, ex.wasm_client_final_view_ptr(), final);
    ok(ex.wasm_client_plan(events.length, final ? 1 : 0), refused);
    return readPlan(ex);
}

/** WHERE THE LAST-BUILT PLAN STANDS at `nowMs` from its start (anim_plan_at). */
export function animPlanAt(nowMs: number): AnimFrameSnap {
    const ex = bots();
    ok(ex.wasm_anim_plan_at(Math.max(0, Math.round(nowMs))), animError('anim_plan_at'));
    const m = mem(ex);
    const at = ex.wasm_anim_frame_ptr();
    const nPlayers = A.AnimFrame_get_n_players(m, at);
    const hand: number[] = [];
    for (let s = 0; s < nPlayers; s++) hand.push(A.AnimFrame_get_hand(m, at, s));
    return {
        step: A.AnimFrame_get_step(m, at),
        elapsedMs: A.AnimFrame_get_elapsed_ms(m, at),
        landed: A.AnimFrame_get_landed(m, at),
        nextMs: A.AnimFrame_get_next_ms(m, at),
        done: A.AnimFrame_get_done(m, at) !== 0,
        deck: A.AnimFrame_get_deck(m, at),
        discard: A.AnimFrame_get_discard(m, at),
        hand, nPlayers,
        flipped: readAnimCard(m, A.AnimFrame_flipped_at(at)),
        inFlightFromDeck: A.AnimFrame_get_in_flight_from_deck(m, at),
        inFlightToFlipped: A.AnimFrame_get_in_flight_to_flipped(m, at),
        veiled: A.AnimFrame_get_veiled(m, at),
        trumpFlight: readAnimCard(m, A.AnimFrame_trump_flight_at(at)),
    };
}

// A Card inside a kernel struct is a packed byte, not the wire's dense id.
// CARD_NONE is what the kernel writes for "no flipped trump left".
function readAnimCard(m: V.Mem, at: number): Card | null {
    const suit = A.Card_get_suit(m, at), value = A.Card_get_value(m, at);
    return suit < 0 || value < 1 ? null : { suit, value };
}

function readPlan(ex: WebExports): AnimPlanSnap {
    const m = mem(ex);
    const at = ex.wasm_anim_plan_ptr();
    const nSteps = A.AnimPlan_get_n_steps(m, at);
    const preAt = A.AnimPlan_pre_at(at);
    const nPlayers = A.AnimCounts_get_n_players(m, preAt);
    const preHand: number[] = [];
    for (let s = 0; s < nPlayers; s++) preHand.push(A.AnimCounts_get_hand(m, preAt, s));
    const nBattles = A.AnimCounts_get_n_battles(m, preAt);
    const battles: number[] = [];
    for (let i = 0; i < 2 * nBattles; i++) battles.push(A.AnimCounts_get_battles(m, preAt, i));
    const steps: AnimPlanStepSnap[] = [];
    for (let i = 0; i < nSteps; i++) {
        const s = A.AnimPlan_steps_at(at, i);
        const hand: number[] = [];
        for (let k = 0; k < nPlayers; k++) hand.push(A.AnimPlanStep_get_hand(m, s, k));
        steps.push({
            type: A.AnimPlanStep_get_type(m, s),
            seat: A.AnimPlanStep_get_seat(m, s),
            from: A.AnimPlanStep_get_from(m, s),
            to: A.AnimPlanStep_get_to(m, s),
            nCards: A.AnimPlanStep_get_n_cards(m, s),
            durationMs: A.AnimPlanStep_get_duration_ms(m, s),
            startMs: A.AnimPlanStep_get_start_ms(m, s),
            beatFirst: A.AnimPlanStep_get_beat_first(m, s),
            beatN: A.AnimPlanStep_get_beat_n(m, s),
            holdMs: A.AnimPlanStep_get_hold_ms(m, s),
            deck: A.AnimPlanStep_get_deck(m, s),
            discard: A.AnimPlanStep_get_discard(m, s),
            hand,
            inFlightFromDeck: A.AnimPlanStep_get_in_flight_from_deck(m, s),
            inFlightToFlipped: A.AnimPlanStep_get_in_flight_to_flipped(m, s),
            reveals: A.AnimPlanStep_get_reveals(m, s),
            trumpOut: readAnimCard(m, A.AnimPlanStep_trump_out_at(s)),
            trumpFrom: A.AnimPlanStep_get_trump_from(m, s),
        });
    }
    const nVeil = A.AnimPlan_get_n_veil(m, at);
    const veilIds: number[] = [];
    for (let i = 0; i < nVeil; i++) veilIds.push(A.AnimPlan_get_veil_ids(m, at, i));
    const nCues = A.AnimPlan_get_n_cues(m, at);
    const cues: AnimCueSnap[] = [];
    for (let i = 0; i < nCues; i++) {
        const c = A.AnimPlan_cues_at(at, i);
        cues.push({
            atMs: A.AnimCue_get_at_ms(m, c),
            kind: A.AnimCue_get_kind(m, c),
            step: A.AnimCue_get_step(m, c),
            hits: A.AnimCue_get_hits(m, c),
            nCards: A.AnimCue_get_n_cards(m, c),
        });
    }
    return {
        nSteps, totalMs: A.AnimPlan_get_total_ms(m, at), steps, veilIds, cues,
        pre: {
            deck: A.AnimCounts_get_deck(m, preAt), discard: A.AnimCounts_get_discard(m, preAt),
            hand: preHand, nPlayers, nBattles, battles,
            paired: A.AnimCounts_get_paired(m, preAt) !== 0,
            flipped: readAnimCard(m, A.AnimCounts_flipped_at(preAt)),
        },
    };
}

/**
 * THE ORDER A DOOMED SEQUENCE FLIES HOME IN (anim_plan.h anim_reversal_order),
 * for a caller that already holds its verdicts - which every server-transport
 * caller does, since anim_conflict_verdict is the only entry that asks the
 * AnimServerHope.
 *
 * `verdicts` are ANIM_CONFLICT_* per motion in the order the motions flew, and
 * `groupSizes` slices them into the parallel steps they flew as. The answer is
 * the steps to play, each a list of motion indices, in REVERSE group order: the
 * cards travel back the way they came, last group first, and a group nothing
 * reverts is dropped rather than played as a beat of silence.
 */
export function animReversalOrder(
    verdicts: readonly number[], groupSizes: readonly number[],
): number[][] {
    const ex = serverBots();
    V.writeAnimReversalAsk(mem(ex), ex.wasm_anim_reversal_ask_ptr(), { verdicts, groupSizes });
    ok(ex.wasm_anim_reversal_order(), animError('anim_reversal_order'));
    const plan = V.readAnimConflictPlan(mem(ex), ex.wasm_anim_reversal_plan_ptr());
    const steps: number[][] = [];
    let at = 0;
    for (const c of plan.stepCount) steps.push(plan.order.slice(at, at += c));
    return steps;
}
