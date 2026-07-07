// CLIENT-SIDE rules kernel (guards.wasm — cnitro/src/game.c only, ~23KB).
//
// The interactive client used to reimplement the move rules in TypeScript
// (src/utils/gameValidation.ts canAttack/canPass/canCoverCards, plus the
// common_utils canCover/get_next_player_index/game_done projections) so it
// could gate buttons and predict optimistic state SYNCHRONOUSLY during render.
// That was a second copy of the rules — guarded by parity tests, but it had
// drifted before. This module removes the duplication: the SAME C engine the
// server runs answers the client's gates and optimistic apply too.
//
// How the sync requirement is met: instantiate ONCE (async — browsers cap
// synchronous main-thread wasm compilation) behind the game-load screen via
// initClientGuards(); afterwards every gate is a synchronous wasm call.
//
// Redacted state: the client only ever validates ITS OWN move and only knows
// its own hand. Opponents' hands are marshaled as hand_length placeholder
// cards — the validators read opponents by COUNT only (capacity), never by
// card identity, so a placeholder is indistinguishable from the real card for
// every gate the client asks. (Optimistic draws from the deck are likewise
// placeholders; the authoritative server broadcast supplies the real cards.)

import { Card, PersonalGame, PublicPlayer, GAME_STATUS, PLAYER_STATUS } from '@shared/types.ts';
import { takeGUARDS_WASM_B64 } from '@shared/wasm/guards_wasm.ts';
// guards embed is gzip+base64 (embed.mjs --gzip); a vendored sync pure-JS
// gunzip inflates it in the browser and keeps the sync instantiate path.
import { gunzip } from '@shared/wasm/gunzip.ts';

// ENGINE_REJECT_* — must match cnitro/src/game.h. 0 == legal.
const REJECT_NONE = 0;

interface GuardsExports {
  memory: WebAssembly.Memory;
  wasm_init(): void;
  wasm_io_ptr(): number;
  wasm_cards_a_ptr(): number;
  wasm_cards_b_ptr(): number;
  wasm_import_state(): void;
  wasm_export_state(): number;
  wasm_validate_attack(seat: number, n: number): number;
  wasm_validate_cover(seat: number, n: number): number;
  wasm_validate_pass(seat: number, n: number): number;
  wasm_validate_pickup(seat: number): number;
  wasm_validate_good(seat: number): number;
  wasm_attack(seat: number, n: number): number;
  wasm_cover(seat: number, n: number): number;
  wasm_pass(seat: number, n: number): number;
  wasm_pickup(seat: number): number;
  wasm_good(seat: number): number;
  // Action-wire entry points (docs/PACKED_WIRE_CUTOVER.md): both read the
  // awire bytes from the cards_a buffer. validate: 0 legal | ENGINE_REJECT_*
  // | -1 malformed. apply: 1 applied | 0 rejected | -1 malformed.
  wasm_validate_action(seat: number, wireLen: number): number;
  wasm_apply_action(seat: number, wireLen: number): number;
  // Masked view blob import (declared for completeness; unused in this pass).
  wasm_import_view(len: number): number;
  wasm_next_player(cur: number): number;
  wasm_game_done(): number;
  wasm_can_cover(as: number, av: number, ds: number, dv: number, ps: number): number;
}

let ex: GuardsExports | null = null;
let loading: Promise<void> | null = null;
let residentFor: PersonalGame | null = null;
// Held from first take until an instantiation SUCCEEDS, so a sync fallback can
// still run after an async attempt decoded the (take-once) embed.
let pendingBytes: Uint8Array | null = null;

const G_STATUS: Record<string, number> = {
  [GAME_STATUS.WAITING]: 0, [GAME_STATUS.PLAYING]: 1, [GAME_STATUS.GAME_OVER]: 2,
};
const P_STATUS: Record<string, number> = {
  [PLAYER_STATUS.IDLE]: 0, [PLAYER_STATUS.READY]: 1, [PLAYER_STATUS.IN]: 2, [PLAYER_STATUS.OUT]: 3,
};

const i8 = (x: number) => (x << 24) >> 24;
// 1-byte wire card, mirrors cnitro/wasm/wire.h + engine.ts wireStateCard.
function wireCard(c: Card): number {
  let s = i8(c.suit & 0xff), v = i8(c.value & 0xff);
  if (s === -1 && v === -1) return 0xfe;
  if (s < 0) s = 0; else if (s > 3) s = 3;
  if (v < 1) v = 1; else if (v > 13) v = 13;
  return s * 13 + (v - 1);
}
const WIRE_NONE = 0xff;
const PLACEHOLDER = 0; // any real card; content is irrelevant for a redacted seat/deck

function guardsBytes(): Uint8Array {
  if (!pendingBytes) pendingBytes = gunzip(decodeB64(takeGUARDS_WASM_B64()));
  return pendingBytes;
}

function adopt(e: GuardsExports): void {
  e.wasm_init();
  ex = e;
  pendingBytes = null;
  residentFor = null;
}

// -------------------------------------------------------------------------
// Load. In a BROWSER, await initClientGuards() once at game-load (behind the
// loading screen) — Chrome caps synchronous main-thread wasm compilation, so
// the async path is mandatory there. In Node / SSR / tests the gates fall back
// to a synchronous instantiate on first use (23KB compiles instantly off the
// main thread), so callers need not await. Mirrors engine.ts.
// -------------------------------------------------------------------------
export function initClientGuards(): Promise<void> {
  if (ex) return Promise.resolve();
  if (!loading) {
    loading = WebAssembly.instantiate(guardsBytes() as BufferSource, {})
      .then(({ instance }) => { if (!ex) adopt(instance.exports as unknown as GuardsExports); })
      .catch((err) => { loading = null; throw err; });
  }
  return loading;
}

export function guardsReady(): boolean { return ex !== null; }

// Diagnostics: current linear-memory footprint in bytes (for perf/mem tests).
// -1 before load. The module uses only fixed static buffers, so this never
// grows once instantiated.
export function guardsMemBytes(): number { return ex ? ex.memory.buffer.byteLength : -1; }

function decodeB64(b64: string): Uint8Array {
  if (typeof atob === 'function') {
    const bin = atob(b64); const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }
  // Node / SSR path.
  return Uint8Array.from(Buffer.from(b64, 'base64'));
}

function bytes(): Uint8Array { return new Uint8Array(ex!.memory.buffer); }

// -------------------------------------------------------------------------
// Marshal a PersonalGame into the kernel's resident state. Skips the write
// when the same game object is already resident (the render pass fires many
// gates against one game). Returns the seat index of `self`.
// -------------------------------------------------------------------------
function seatOfSelf(g: PersonalGame): number {
  const id = g.self?.player_id;
  const s = g.players.findIndex((p) => p.player_id === id);
  return s < 0 ? 0 : s;
}

function marshal(g: PersonalGame): void {
  if (residentFor === g) return;
  const buf = bytes();
  let q = ex!.wasm_io_ptr();
  const selfSeat = seatOfSelf(g);

  buf[q++] = G_STATUS[g.status] ?? 0;
  buf[q++] = g.players.length;
  buf[q++] = g.power_suit & 0xff;
  buf[q++] = g.first_attacker & 0xff;
  buf[q++] = g.defender & 0xff;
  buf[q++] = g.discard_pile_length & 0xff;
  buf[q++] = (g.discard_pile_length >> 8) & 0xff;
  buf[q++] = g.flipped ? 1 : 0;
  buf[q++] = g.flipped ? wireCard(g.flipped) : 0;
  let mask = 0;
  for (const pid of g.good_players ?? []) {
    const s = g.players.findIndex((p) => p.player_id === pid);
    if (s >= 0) mask |= 1 << s;
  }
  buf[q++] = mask & 0xff; buf[q++] = (mask >> 8) & 0xff;
  buf[q++] = (mask >> 16) & 0xff; buf[q++] = (mask >> 24) & 0xff;
  buf[q++] = g.good_timestamp != null ? 1 : 0;
  // Deck: only the COUNT is authoritative to the client; cards are placeholders.
  const deckLen = g.deck_length ?? 0;
  buf[q++] = deckLen & 0xff; buf[q++] = (deckLen >> 8) & 0xff;
  for (let i = 0; i < deckLen; i++) buf[q++] = PLACEHOLDER;
  buf[q++] = g.table_battles.length;
  for (const b of g.table_battles) {
    buf[q++] = wireCard(b.attack);
    buf[q++] = b.defense ? wireCard(b.defense) : WIRE_NONE;
  }
  for (let s = 0; s < g.players.length; s++) {
    const p = g.players[s] as PublicPlayer;
    buf[q++] = P_STATUS[p.status] ?? 0;
    buf[q++] = (p as any).awaiting_attack ? 1 : 0;
    // The written hand_count MUST equal the number of cards we actually write,
    // or the wire desyncs. For self we write its real hand; for opponents we
    // write hand_length placeholders (they read by count only). In a live game
    // self.hand.length === self.hand_length; they can differ only in synthetic
    // fixtures, where the real cards win.
    const selfHand = (s === selfSeat && g.self?.hand) ? g.self.hand : null;
    const count = selfHand ? selfHand.length : (p.hand_length ?? 0);
    buf[q++] = count & 0xff;
    if (selfHand) for (const c of selfHand) buf[q++] = wireCard(c);
    else for (let i = 0; i < count; i++) buf[q++] = PLACEHOLDER; // redacted opponent
  }
  const elim = g.elimination_order ?? [];
  buf[q++] = elim.length;
  for (const pid of elim) {
    const s = g.players.findIndex((p) => p.player_id === pid);
    buf[q++] = s & 0xff;
  }

  ex!.wasm_import_state();
  residentFor = g;
}

function writeCards(ptr: number, cards: Card[]): void {
  const buf = bytes();
  for (let i = 0; i < cards.length; i++) buf[ptr + i] = wireCard(cards[i]);
}

function ensure(): GuardsExports {
  if (ex) return ex;
  // Synchronous fallback (Node / SSR / tests). In a browser main thread this
  // throws for a >4KB module; the app must await initClientGuards() at mount.
  try {
    const instance = new WebAssembly.Instance(new WebAssembly.Module(guardsBytes() as BufferSource));
    adopt(instance.exports as unknown as GuardsExports);
    return ex!;
  } catch (e) {
    throw new Error('clientGuards not initialized — await initClientGuards() at game load ('
      + (e as Error).message + ')');
  }
}

// -------------------------------------------------------------------------
// UI gates (synchronous once loaded). Return true when the move is LEGAL by
// the authoritative rules — the exact question the button/drag code asks.
// -------------------------------------------------------------------------
export function canAttack(g: PersonalGame, cards: Card[]): boolean {
  const e = ensure();
  if (cards.length === 0) return false;
  marshal(g);
  writeCards(e.wasm_cards_a_ptr(), cards);
  return e.wasm_validate_attack(seatOfSelf(g), cards.length) === REJECT_NONE;
}

export function canPass(g: PersonalGame, cards: Card[]): boolean {
  const e = ensure();
  if (cards.length === 0) return false;
  marshal(g);
  writeCards(e.wasm_cards_a_ptr(), cards);
  return e.wasm_validate_pass(seatOfSelf(g), cards.length) === REJECT_NONE;
}

export function canCover(g: PersonalGame, coverCards: Card[], attackCards: Card[]): boolean {
  const e = ensure();
  if (coverCards.length === 0 || coverCards.length !== attackCards.length) return false;
  marshal(g);
  writeCards(e.wasm_cards_a_ptr(), coverCards);
  writeCards(e.wasm_cards_b_ptr(), attackCards);
  return e.wasm_validate_cover(seatOfSelf(g), coverCards.length) === REJECT_NONE;
}

export function canPickup(g: PersonalGame): boolean {
  const e = ensure();
  marshal(g);
  return e.wasm_validate_pickup(seatOfSelf(g)) === REJECT_NONE;
}

// Pure primitives the client used to keep in common_utils.
export function canCoverPair(attack: Card, defense: Card, powerSuit: number): boolean {
  return ensure().wasm_can_cover(attack.suit, attack.value, defense.suit, defense.value, powerSuit) === 1;
}

export function nextPlayerIndex(g: PersonalGame, current: number): number {
  const e = ensure();
  marshal(g);
  return e.wasm_next_player(current);
}

export function gameDone(g: PersonalGame): number {
  const e = ensure();
  marshal(g);
  return e.wasm_game_done(); // seat index of the loser, or -1
}

// -------------------------------------------------------------------------
// Optimistic apply: run a SELF move through the mutating kernel and read back
// the PREDICTED next state — the authoritative rules applied client-side, so
// the overlay never diverges from the server's own transition (e.g. the
// eliminated-seat defender rotation the hand-rolled TS optimistic path got
// wrong). Returns null if the kernel rejects the move.
//
// Caveat (by design): any cards DRAWN from the stock during the move (a
// round-ending cover/pickup that triggers a refill) are unknown to the client
// and come back as placeholders; the authoritative server broadcast supplies
// the real draws. The common case (attack / cover / pass mid-bout) draws
// nothing and predicts exactly.
// -------------------------------------------------------------------------
const G_STATUS_FROM = [GAME_STATUS.WAITING, GAME_STATUS.PLAYING, GAME_STATUS.GAME_OVER] as const;
const P_STATUS_FROM = [PLAYER_STATUS.IDLE, PLAYER_STATUS.READY, PLAYER_STATUS.IN, PLAYER_STATUS.OUT] as const;
const cardFromWire = (b: number): Card => b === 0xfe
  ? { suit: -1, value: -1 }
  : { suit: Math.floor((b <= 51 ? b : 51) / 13), value: (b <= 51 ? b : 51) % 13 + 1 };

export interface OptimisticMove {
  type: 'attack' | 'cover' | 'pass' | 'pickup';
  cards?: Card[];
  attack_cards?: Card[];
}

// -------------------------------------------------------------------------
// Wire-based entry points. The client builds ONE awire buffer per move
// (@shared/wire/awire.ts encodeAction) and uses it for the gate, the
// optimistic apply AND the POST body — the kernel judging the move here
// decodes the exact bytes the server kernel will apply.
// -------------------------------------------------------------------------

// 0 = legal; else the ENGINE_REJECT_* code, or -1 for a malformed wire.
export function validateActionWire(g: PersonalGame, wire: Uint8Array): number {
  const e = ensure();
  marshal(g);
  bytes().set(wire, e.wasm_cards_a_ptr());
  return e.wasm_validate_action(seatOfSelf(g), wire.length);
}

// Optimistic apply straight from the awire bytes — same predicted-state
// semantics (and drawn-cards-as-placeholders caveat) as applyMove below.
// Null on reject or malformed wire.
export function applyMoveWire(g: PersonalGame, wire: Uint8Array): PersonalGame | null {
  const e = ensure();
  marshal(g);
  const seat = seatOfSelf(g);
  bytes().set(wire, e.wasm_cards_a_ptr());
  const ok = e.wasm_apply_action(seat, wire.length);
  residentFor = null; // apply mutated (or may have partially advanced) the resident game
  if (ok !== 1) return null;
  e.wasm_export_state();
  return readState(g, seat);
}

export function applyMove(g: PersonalGame, move: OptimisticMove): PersonalGame | null {
  const e = ensure();
  marshal(g);
  const seat = seatOfSelf(g);
  let ok = 0;
  switch (move.type) {
    case 'attack': writeCards(e.wasm_cards_a_ptr(), move.cards!); ok = e.wasm_attack(seat, move.cards!.length); break;
    case 'pass':   writeCards(e.wasm_cards_a_ptr(), move.cards!); ok = e.wasm_pass(seat, move.cards!.length); break;
    case 'cover':
      writeCards(e.wasm_cards_a_ptr(), move.cards!);
      writeCards(e.wasm_cards_b_ptr(), move.attack_cards!);
      ok = e.wasm_cover(seat, move.cards!.length); break;
    case 'pickup': ok = e.wasm_pickup(seat); break;
  }
  residentFor = null; // apply mutated the resident game
  if (!ok) return null;
  e.wasm_export_state();
  return readState(g, seat);
}

// Parse the kernel's exported state (put_state layout) back into a PersonalGame,
// borrowing player identity (ids/names) from the pre-move template.
function readState(template: PersonalGame, selfSeat: number): PersonalGame {
  const buf = bytes();
  let q = ex!.wasm_io_ptr();
  const u8 = () => buf[q++];
  const i8v = () => i8(buf[q++]);

  const status = G_STATUS_FROM[u8()] ?? GAME_STATUS.PLAYING;
  const np = u8();
  const power_suit = i8v();
  const first_attacker = i8v();
  const defender = i8v();
  const discard = u8() | (u8() << 8);
  const hasFlipped = u8() !== 0;
  const flippedWire = u8();
  const flipped = hasFlipped ? cardFromWire(flippedWire) : null;
  const mask = u8() | (u8() << 8) | (u8() << 16) | (u8() << 24);
  const hasGoodTs = u8() !== 0;
  const deckLen = u8() | (u8() << 8);
  q += deckLen; // deck cards are placeholders to the client
  const nb = u8();
  const table_battles = [];
  for (let i = 0; i < nb; i++) {
    const attack = cardFromWire(u8());
    const dw = u8();
    table_battles.push({ attack, defense: dw === WIRE_NONE ? null : cardFromWire(dw) });
  }
  const players = template.players.map((p) => ({ ...p }));
  let selfHand: Card[] = template.self?.hand ?? [];
  for (let s = 0; s < np; s++) {
    const st = P_STATUS_FROM[u8()] ?? p_status_default;
    const awaiting = u8() !== 0;
    const hc = u8();
    const cards: Card[] = [];
    for (let j = 0; j < hc; j++) cards.push(cardFromWire(u8()));
    if (players[s]) {
      (players[s] as any).status = st;
      (players[s] as any).awaiting_attack = awaiting;
      (players[s] as any).hand_length = hc;
    }
    if (s === selfSeat) selfHand = cards; // self's cards are real
  }
  const nElim = u8();
  const elimination_order: string[] = [];
  for (let i = 0; i < nElim; i++) {
    const s = i8(buf[q++]);
    if (template.players[s]) elimination_order.push(template.players[s].player_id);
  }
  const good_players: string[] = [];
  for (let s = 0; s < np; s++) if (mask & (1 << s)) good_players.push(template.players[s].player_id);

  return {
    ...template,
    status, power_suit, first_attacker, defender,
    discard_pile_length: discard, flipped, deck_length: deckLen,
    good_timestamp: hasGoodTs ? (template.good_timestamp ?? Date.now()) : null,
    good_players, elimination_order,
    table_battles: table_battles as PersonalGame['table_battles'],
    players: players as PersonalGame['players'],
    self: { ...template.self, hand: selfHand, hand_length: selfHand.length },
  };
}
const p_status_default = PLAYER_STATUS.IN;
