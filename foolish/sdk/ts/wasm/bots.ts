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

// Memory diagnostics for the edge 150MB-external budget: the size of the
// kernel's linear memory, -1 until loaded (e2e/bench_bot_e2e.ts reads both).
export function __botsWasmBytes(): number {
    return exportsCache ? exportsCache.memory.buffer.byteLength : -1;
}
export function __botsWasmMB(): number {
    return exportsCache ? Math.round(__botsWasmBytes() / 1048576) : -1;
}

/** Instantiate bots.wasm now (a harness that measures memory or timing before its first call). */
export function __ensureBots(): void { bots(); }

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
// leaves out (sealing, rebasing, encoding: what a server and a test do). Where
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
const codeTooBig = () => __replayError(-K.REPLAY_ECAP);
const tooBig = (what: string) => () => new Error(`${what}: does not fit the kernel IO buffer`);

/** A replay refusal as a sentence. `d` is its parameters (replay.h ReplayError),
 *  which fill the table's {version}, {log_type} and {menu}; zeros stand in when
 *  a caller has only the code. */
export function __replayError(negCode: number, d: V.ReplayError_Snap = { version: 0, logType: 0, menu: 0 }): Error {
    return new Error(refusalText(ReplayErrorText, negCode, 'replay')
        .replace('{version}', `${d.version}`).replace('{menu}', `${d.menu}`)
        .replace('{log_type}', K.LOG_NAMES[d.logType]?.toLowerCase() ?? `${d.logType}`));
}

/** The module's last replay refusal, with the parameters the kernel kept for it. */
function replayRefused(negCode: number): Error {
    const ex = bots();
    return __replayError(negCode, V.readReplayError(mem(ex), ex.wasm_replay_error_ptr()));
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

/** The STRAT_* brain id the kernel's roster gives this key, or -1. */
export function kernelBotStrat(key: string): number {
    return kernelBotRoster().find((e) => e.key === key)?.strat ?? -1;
}

// ---------- FMSG: the iMessage envelope (c/src/msg_wire.h) -------------
//
// An iMessage game has no server: the whole game is (32-byte deal seed, v6
// replay code) in an MSMessage URL, and every device rebuilds it by re-dealing
// from the seed and replaying the code through the kernel. These calls are the
// only way in - the envelope's layout lives in C, once, so the phone and the
// web can never read the same bytes as different games.
//
// They ride the REPLAY io buffer, never g_io: an FMSG call IS a replay call (its
// body is a v6 code), so an envelope in g_io would be clobbered by the codec's
// own bignum scratch mid-decode. See wasm_api.c.

// Decode + VALIDATE an envelope: the chain is replayed through the kernel, so a
// corrupt or hand-edited payload throws rather than half-loading (§7.3 -
// validation IS replay). Leaves the game RESIDENT: the legal-move and view doors
// below then read exactly the state the payload describes, which is what the
// /m/ route renders and what a turn continues from.
//
// The header comes back as the C struct itself (msg_wire.h MsgHeader, at
// wasm_msg_header_ptr), read by its generated reader: the envelope is `.e` and
// the digest Rule P breaks ties on is `.digest`.
export function kernelMsgDecode(envelope: Uint8Array): M.MsgHeader_Snap {
    const ex = replayIn(bots(), msgTooBig, envelope);
    ok(ex.wasm_msg_decode(envelope.length), msgError);
    return M.readMsgHeader(mem(ex), ex.wasm_msg_header_ptr());
}

// Rule P (§7.2): which chain does every device prefer? <0 a, >0 b, 0 the same.
//
// The comparison is in C (msg_rule_p) and not here, deliberately. This decides
// which game every player sees; a browser and a phone disagreeing forks the
// game, so there is one implementation and nothing to port to Swift. TS only
// moves bytes.
export function kernelMsgRuleP(a: Uint8Array, b: Uint8Array): number {
    const r = replayIn(serverBots(), msgTooBig, a, b).wasm_msg_rule_p(a.length, b.length);
    // -1/0/+1 are verdicts; anything below is a decode error (MSG_E* < -1).
    if (r < -1) throw msgError(r);
    return r;
}

// The legal moves of the game the last kernelMsgDecode adopted - read from the
// kernel's own LegalMoves where it lies, through the generated accessors, never
// marshalled. An iMessage device does not hold the game as a TS object: the
// envelope put it in the kernel, and that is the only copy.
//
// This is how the extension/route answers "what can I do?" - never by asking TS.
// A hand-rolled "is it my turn" is a bug by policy (design §17.16).
export function kernelMsgLegalMoves(seat: number): { type: string; cards?: Card[]; attack_cards?: Card[] }[] {
    const ex = serverBots();
    const total = ex.wasm_legal_moves(seat);
    const m = mem(ex), lm = ex.wasm_legal_moves_ptr();
    const card = (p: number): Card => ({ suit: A.Card_get_suit(m, p), value: A.Card_get_value(m, p) });
    return Array.from({ length: total }, (_, i) => {
        const mv = A.LegalMoves_moves_at(lm, i);
        const code = A.LegalMove_get_type(m, mv);
        const type = K.MOVE_NAMES[code].toLowerCase();
        if (code === K.MOVE_PICKUP || code === K.MOVE_GOOD || code === K.MOVE_WAIT) return { type };
        const cards = Array.from({ length: A.LegalMove_get_n_cards(m, mv) }, (_c, j) => card(A.LegalMove_cards_at(mv, j)));
        if (code !== K.MOVE_COVER) return { type, cards };
        return { type, cards, attack_cards: cards.map((_c, j) => card(A.LegalMove_attack_cards_at(mv, j))) };
    });
}

// Rule R (§7.4): rebase ONE pending action onto the chain kernelMsgDecode last
// adopted, in ledger order; the answer is a MSG_REBASE_*. REAPPLY mutates the
// resident game - that IS the rebase - so keep folding the rest in after it; a
// DISCARD leaves it untouched.
//
// The round-boundary guard and the legality question both live in C. A throw-in
// composed against round 5, after a pickup closed round 5, would re-validate as
// an opening attack of round 6 - legal, and not what the player chose.
export function kernelMsgRebase(pendingRound: number, seat: number, wire: Uint8Array): number {
    const ex = replayIn(serverBots(), () => msgError(M.MSG_EACTION), wire);
    return ok(ex.wasm_msg_rebase(pendingRound, seat, wire.length), msgError);
}

// The PUBLIC view of the game the last kernelMsgDecode adopted - every hand as
// backs, the deck masked. What /m/ renders for a stranger with a link, and what
// the bubble snapshot shows (it lands in notifications and on lock screens, so
// it must never carry a hand - design §5 invariants).
//
// The resident game is read into the web client's slot as a spectator sees it
// (c/src/client_table.h client_adopt_board), so this needs no re-deserialize:
// the envelope already put the game in the kernel. The masking itself is in
// view.c, like every other view in the product - nothing here decides what a
// stranger may see. The view names no one: the joins carry the names.
export function kernelMsgPublicView(): { view: V.TableView_Snap } {
    const ex = serverBots();
    const rc = ex.wasm_client_adopt_resident(-1);
    if (rc !== 0) throw new Error(`view: the resident game does not read (${rc})`);
    return { view: V.readTableView(mem(ex), ex.wasm_client_view_ptr()) };
}

// Seal the RESIDENT game into an envelope. `header` supplies what the protocol
// owns (game_id, phase, seed, joins, parent8, last_actor_seat); the kernel fills
// in what the BODY owns - turn, round - by decoding the code it just wrote, so a
// host cannot emit a payload it would itself reject. Returns the wire bytes.
/** What a caller states of an envelope it seals: the protocol's fields. The
 *  kernel derives the rest from the chain wasm_msg_decode adopted. */
type MsgSealHeader =
    Omit<M.MsgEnvelope_Snap, 'format' | 'turn' | 'round' | 'nNew' | 'sentAt' | 'opening' | 'carryKey' | 'carryFool' | 'parent8' | 'seed'>
    & Partial<Pick<M.MsgEnvelope_Snap, 'sentAt' | 'opening' | 'carryKey' | 'carryFool'>>
    & { parent8: ArrayLike<number>; seed: ArrayLike<number> };

export function kernelMsgSeal(header: MsgSealHeader): Uint8Array {
    const ex = serverBots();
    const at = ex.wasm_msg_header_ptr();
    // The format here is a placeholder: msg_seal picks the real one off what the
    // header ends up carrying (a clock, or a bubble delta, seals format 3), so
    // the caller never states it twice. n_new is 0 and the digest is left as it
    // lay for the same reason: the kernel derives both, from the chain
    // wasm_msg_decode adopted. A name too long for its slot is the writer's
    // RangeError.
    M.writeMsgHeader(mem(ex), at, {
        e: {
            ...header, format: M.MSG_FORMAT_V6, round: 0, turn: 0, nNew: 0,
            parent8: Array.from(header.parent8).slice(0, M.MSG_PARENT_LEN),
            seed: Array.from(header.seed).slice(0, M.MSG_SEED_LEN),
            sentAt: (header.sentAt ?? 0) & 0xffff,
            opening: header.opening ?? M.MSG_NO_OPENING,
            carryKey: (header.carryKey ?? 0) >>> 0, carryFool: header.carryFool ?? M.MSG_NO_FOOL,
        },
        digest: M.readMsgHeader(mem(ex), at).digest,
    });
    return taken(ex, ok(ex.wasm_msg_seal(), msgError), ex.wasm_replay_io_ptr());
}

// THE REMATCH LOBBY for a FINISHED envelope (c/src/msg_wire.h
// msg_rematch_lobby): the same game dealt again, built wholly by the kernel.
// The same C the phone's fio_msg_rematch calls - this is how
// e2e/msg_rematch.test.ts holds the two engines to one answer.
export function kernelMsgRematch(finished: Uint8Array, tappedAtMs: number, creatorSeat: number): Uint8Array {
    const ex = replayIn(serverBots(), msgTooBig, finished);
    return taken(ex, ok(ex.wasm_msg_rematch(finished.length, tappedAtMs, creatorSeat), msgError), ex.wasm_replay_io_ptr());
}

// The best shareable REPLAY code for the game the last kernelMsgDecode
// adopted - the TS-side twin of MessageKernel.residentReplayCode()
// (sdk/swift/MessageEnvelope.swift), reached off the same resident g_game
// instead of Swift's fio_replay_share_code_b32. Used by the /m/ page's
// FINISHED-bubble funnel (docs/IMESSAGE_LOBBY_V3.md, batch 6 item B): once a
// payload decodes, msg_replay has already run the whole chain through the
// ORDINARY kernel handlers (handle_attack etc.), which log exactly like any
// other play - so the resident game already carries the full session log a
// v6 code needs. Only the envelope's own seed (env.seed, already decoded - no
// second kernel round-trip to fetch it) has to be supplied.
export function kernelResidentReplayCodeV6(seed: Uint8Array): Uint8Array {
    if (seed.length !== M.MSG_SEED_LEN) {
        throw new Error(`replay: v6 needs a ${M.MSG_SEED_LEN}-byte deal seed, got ${seed.length}`);
    }
    const ex = replayIn(serverBots(), codeTooBig, seed);
    return taken(ex, ok(ex.wasm_replay_encode_v6_from_game(1 << 30), replayRefused), ex.wasm_replay_io_ptr());
}

/* ---------------- the replay code's extras blob (c/src/replay_extras.h) ------
 *
 * The nicknames and per-move timing behind the dash in a share link. The codec
 * is the kernel's - one encoder, shared with the phone (fio_replay_extras_link)
 * - and these are the doors the web and the server reach it through. Both are
 * synchronous and assume a warm module, like every other reader here. Callers
 * catch an extras failure and fall back to "P1"/"P2".
 */

/**
 * The names and timing into the kernel's own struct (replay_extras.h
 * ReplayExtras) at wasm_replay_extras_ptr, through the generated writer. The
 * codec's packed argument blob is built from it IN C (replay_extras_pack), so
 * nothing here knows its layout; a name too wide for the struct's slot, or more
 * gaps than it holds, is the writer's RangeError.
 */
function extrasArgs(ex: WebExports, names: string[] | null, startTime: number | null, gaps: number[] | null): void {
    const hasNames = (names?.length ?? 0) > 0;
    const hasTimes = startTime !== null && gaps !== null;
    M.writeReplayExtras(mem(ex), ex.wasm_replay_extras_ptr(), {
        flags: (hasNames ? M.REPLAY_EXTRAS_FLAG_NAMES : 0) | (hasTimes ? M.REPLAY_EXTRAS_FLAG_TIMES : 0),
        names: hasNames ? names!.map((text) => ({ text })) : [],
        startTime: hasTimes ? startTime! : 0,
        gaps: hasTimes ? gaps! : [],
    });
}

/**
 * Roster + timing -> the raw extras blob. `names` must be as wide as the table
 * (a reader takes the seat count from the decoded moves); unnamed seats are ''.
 * Passing null for either half leaves that section out.
 */
export function kernelReplayExtrasEncode(names: string[] | null, startTime: number | null,
                                         gaps: number[] | null): Uint8Array {
    const ex = serverBots();
    extrasArgs(ex, names, startTime, gaps);
    return taken(ex, ok(ex.wasm_replay_extras_encode(), extrasError));
}

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
    extrasArgs(ex, names, null, null);
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
export function replayStepCount(code: Uint8Array): number {
    return ok(codeIn(code).wasm_replay_step_count(code.length), replayRefused);
}

/** What a step played: the kernel's REPLAY_ATOM_* (c/src/replay.h), each under
 *  its own name with the prefix dropped, out of the generated constants. DEAL is
 *  the opening deal and never an action; DRAW is never a step (draws ride the
 *  action that caused them); ROUND_END is every remaining attacker saying good. */
type AtomName<N> = N extends `REPLAY_ATOM_${infer S}` ? (S extends 'NAMES' ? never : S) : never;
export const REPLAY_STEP = Object.fromEntries(
    Object.entries(K.REPLAY_ATOM_NAMES).map(([code, name]) => [name, Number(code)]),
) as { readonly [N in keyof typeof K as AtomName<N>]: (typeof K)[N] };

/** What one step of a code IS (replay_steps.h ReplayStep): `kind` a REPLAY_ATOM_*,
 *  `seat` the acting seat or -1 (the deal, and round ends nobody in particular
 *  closes), `named` how many of the step's cards the MOVE NAMED - 0 for a pickup
 *  or a good, whose step carries cards nobody chose. */
export type ReplayStepInfo = V.ReplayStep_Snap;

/**
 * What each step of a code IS, in step order. The kernel reports this rather
 * than the web inferring it from the frames, because the frames genuinely
 * cannot say: an attack and a pass are the same evwire event type, told apart
 * only by a reconstructed English message. See replay_steps.h.
 */
export function replayStepIndex(code: Uint8Array): ReplayStepInfo[] {
    const ex = codeIn(code);
    const len = ok(ex.wasm_replay_step_index(code.length), replayRefused);
    const m = mem(ex), base = ex.wasm_io_ptr();
    return Array.from({ length: len / K.RS_INDEX_STRIDE }, (_, i) => V.readReplayStep(m, base + i * K.RS_INDEX_STRIDE));
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
// AnimHandAsk reads it as a back on the way in and writes one on the way out),
// and so does a board whose flipped trump has been dealt out (AnimPlanAsk).
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

/** One decoded event as the plan sees it (anim_plan.h AnimPlanEvent). */
export interface AnimPlanEventIn {
    type: number;                   // ANIM_EVT_*
    seat?: number;                  // the acting seat; absent for none
    from?: number; to?: number;     // ANIM_LOC_*
    cards?: readonly Card[];
    maskCards?: boolean;            // viewer-masked backs: no identity, no veil
    /** THIS step's own board, when the wire carried one. */
    counts?: { deck: number; discard: number; flipped: Card | null; hand: readonly number[] };
    /** The row that board held, 2 bytes per battle (attack, then its cover or ANIM_TABLE_NONE). */
    battles?: readonly number[];
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

/** The plan (anim_plan.h AnimPlan). */
export interface AnimPlanSnap {
    nSteps: number; totalMs: number; pre: AnimCountsSnap;
    veilIds: number[]; steps: AnimPlanStepSnap[];
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

/** THE TIMED PLAN for a decoded viewer sequence (anim_plan.h anim_build_plan).
 *  `final` is the board the host already holds; the plan freezes the DISPLAY
 *  back to the pre-sequence values and reveals forward one step at a time. */
export function animBuildPlan(
    events: readonly AnimPlanEventIn[], nPlayers: number,
    final: { deck: number; discard: number; flipped: Card | null; hand: readonly number[] },
): AnimPlanSnap {
    const ex = bots();
    // A hand as the ask holds one: a count for every seat a table can have,
    // of which the kernel reads this table's own.
    const seats = (hand: readonly number[]): number[] =>
        Array.from({ length: V.MAX_PLAYERS }, (_, s) => hand[s] ?? 0);
    V.writeAnimPlanAsk(mem(ex), ex.wasm_anim_plan_ask_ptr(), {
        nPlayers,
        finalDeck: final.deck, finalDiscard: final.discard,
        finalFlipped: final.flipped ?? NO_CARD, finalHand: seats(final.hand),
        events: events.map((e) => ({
            type: e.type,
            seat: e.seat === undefined || e.seat < 0 ? A.ANIM_SEAT_NONE : e.seat,
            from: e.from ?? A.ANIM_LOC_NONE, to: e.to ?? A.ANIM_LOC_NONE,
            maskCards: !!e.maskCards,
            cards: e.cards ?? [],
            hasCounts: !!e.counts,
            deck: e.counts?.deck ?? 0, discard: e.counts?.discard ?? 0,
            flipped: e.counts?.flipped ?? NO_CARD, hand: seats(e.counts?.hand ?? []),
            row: e.battles ?? [],
        })),
    });
    ok(ex.wasm_anim_build_plan(), animError('anim_build_plan'));
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
    return {
        nSteps, totalMs: A.AnimPlan_get_total_ms(m, at), steps, veilIds,
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
