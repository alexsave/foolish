/* =============================================================================
 * foolish.cards — whole-game replay format v1: shared core (model + menus)
 * =============================================================================
 * Encodes a finished game's log stream into a single integer (see codec.ts).
 *
 * HOW IT WORKS
 * ------------
 * Both encoder and decoder run the same deterministic PUBLIC-STATE replayer —
 * a faithful projection of the server engine (supabase/functions/_shared/
 * actions/*.ts + common_utils.ts) onto what a spectator can see: hand COUNTS,
 * publicly-revealed cards, the table, statuses. At every point where
 * information enters the game, both sides build the identical menu of options
 * and the coder stores/retrieves only "which option happened".
 *
 * Card identities are coded LAZILY: a hidden card costs bits only the first
 * time it is publicly seen (played). Draws are free (recipient and count are
 * derived from the refill rules; identities stay hidden). Cards that are never
 * played are never coded at all. This matches the game_logs table exactly —
 * DRAW logs already store {suit:-1, value:-1} for hidden cards — so the client
 * can encode a game it merely spectated.
 *
 * Derived events (DISCARD, DRAW, DEFENDER_CHANGE, PLAYER_OUT) cost zero bits:
 * the replayer regenerates them from the rules. Only ATTACK / COVER / PASS /
 * PICKUP / GOOD logs carry information.
 *
 * DETERMINISM CONTRACT (do not break — see files/HANDOFF.md §4)
 * - Menus are a pure function of public state: fixed seat order, battles in
 *   table order, cards in PREFERENCE order (non-trumps by ascending value
 *   then suit, then trumps likewise — see prefSorted).
 * - The weights (V1 profile below) are wire format. Never change menus or
 *   weights without bumping FORMAT_VERSION and keeping the old code path.
 * - The decoder stops when the RULES say the game is over (one player left
 *   IN); the integer never decides termination.
 *
 * PROBABILITY MODEL (v1, frozen)
 * - Decision weights: geometric decay along the preference order (good play
 *   usually spends the cheapest card), a strong "good" once everything is
 *   covered, and a strong STOP on multi-card continuations.
 * - Fresh-card identities: hypergeometric "best of the player's u hidden
 *   cards out of the U unseen" weights C(U-1-j, u-1) over the
 *   preference-ordered feasible list, quantized to ID_QUANT. This is what
 *   makes reveals cheap — low cards come out first, and the model knows it.
 * ========================================================================== */

import { Card, LOG_TYPE, LogType, LogCardPair } from "../common/types";
import { ACE_VALUE, CARDS_PER_PLAYER } from "../common/constants";
import { canCover } from "../common/common_utils";
import { Coder, comb } from "./codec";

export const FORMAT_VERSION = 1;
export const VERSION_ALPHABET = 16; // room for 15 future versions before a re-think
const MAX_ATOMS = 20000; // hard guard: a malformed integer must never hang

/* v1 weight profile — FROZEN wire format (bump FORMAT_VERSION to change).
 * Tuned on engine-driven random + handwritten bot games; every legal option
 * keeps weight >= 1 so any legal game stays encodable. */
const V1 = {
  COVER: 48, // best-known cover, geometric decay (>>1 per pref position)
  COVER_FRESH: 48, // cover from a hidden card
  PASS: 16, // perevod with a known card, geometric decay
  PASS_FRESH: 16,
  PICKUP: 8,
  ATTACK: 12, // known-card attack/throw-in, geometric decay
  ATTACK_FRESH_LEAD: 32, // leading a bout from a hidden card (the usual case)
  ATTACK_FRESH: 8, // throwing in from a hidden card
  GOOD_COVERED: 64, // "good" once every attack is covered (ends the round)
  GOOD_OPEN: 8, // declining to act while attacks are still open
  STOP: 10, // continuation menus: stop after the current card
  ID_QUANT: 16384, // quantization of hypergeometric identity weights
} as const;

function geo(base: number, pos: number): number {
  return Math.max(base >> pos, 1);
}

/* ------------------------------- card ids -------------------------------- */
// id = suit*13 + (value-1), ascending = (suit, value) order. Values are 1..13
// (2..A) on the 52-card deck (5+ players) and 5..13 (6..A) on the 36-card deck.

const HIDDEN: Card = { suit: -1, value: -1 };

export function cardId(c: Card): number {
  return c.suit * 13 + (c.value - 1);
}
export function idToCard(id: number): Card {
  return { suit: Math.floor(id / 13), value: (id % 13) + 1 };
}
function idValue(id: number): number {
  return (id % 13) + 1;
}

/* ------------------------------- io types -------------------------------- */

/** The slice of a GameLog the codec needs (ids/timestamps are irrelevant). */
export interface ReplayLogEntry {
  log_type: LogType;
  player_id: string | null;
  card_pairs: LogCardPair[];
  defender_index: number | null;
}

export interface ReplayInput {
  /** player_ids in seat order — game.players order, NOT elimination order */
  playerIds: string[];
  /** logs of the session, ascending in time (may include older sessions —
   *  everything before the last GAME_START is ignored) */
  logs: ReplayLogEntry[];
  /** game.flipped at encode time; null if the trump card was drawn (its
   *  identity is then recovered from the DRAW log that revealed it) */
  flipped: Card | null;
}

/** A reconstructed log entry; seats replace player ids (the integer is
 *  self-contained and carries no identity data). */
export interface SeatLog {
  log_type: LogType;
  seat: number | null;
  card_pairs: LogCardPair[];
  defender_index: number | null;
}

export interface DecodedReplay {
  formatVersion: number;
  playerCount: number;
  trumpCard: Card;
  powerSuit: number;
  firstAttacker: number;
  /** full reconstructed event stream, GAME_START through the final event,
   *  including all derived DISCARD/DRAW/DEFENDER_CHANGE/PLAYER_OUT entries */
  logs: SeatLog[];
  /** seats in the order they went out; the fool is not in this list */
  eliminationOrder: number[];
  fool: number;
  discardPileLength: number;
}

export interface EncodedReplay {
  x: bigint;
  bytes: Uint8Array;
  byteLength: number;
  base32: string;
  base64: string;
  url: string;
}

/* ----------------------------- public model ------------------------------ */

export interface Model {
  n: number;
  trumpId: number;
  powerSuit: number;
  status: boolean[]; // true = IN
  known: Set<number>[]; // per seat: revealed cards currently in hand
  unknown: number[]; // per seat: count of hidden cards
  unseen: Set<number>; // never-revealed cards (face-down stock + hidden in hands)
  deckCount: number; // face-down stock (excludes the flipped trump)
  flippedHeld: boolean; // trump card still waiting under the stock
  battles: { attack: number; defense: number | null }[];
  firstAttacker: number;
  defender: number;
  goods: Set<number>;
  eliminationOrder: number[];
  discard: number;
  out: SeatLog[];
}

function handLen(m: Model, s: number): number {
  return m.known[s].size + m.unknown[s];
}
function stockTotal(m: Model): number {
  return m.deckCount + (m.flippedHeld ? 1 : 0);
}
export function inCount(m: Model): number {
  let c = 0;
  for (const s of m.status) if (s) c++;
  return c;
}

// Faithful port of get_next_player_index (common_utils.ts), including the
// "<= 1 player left returns current" guard — derived DEFENDER_CHANGE indexes
// at game end depend on it.
function nextIn(m: Model, cur: number): number {
  if (inCount(m) <= 1) return cur;
  let nx = (cur + 1) % m.n;
  while (!m.status[nx]) nx = (nx + 1) % m.n;
  return nx;
}

// Preference order: cheap cards first — non-trumps ascending by (value,
// suit), then trumps ascending. Menu enumeration order AND the weight decay
// both follow it, so this ordering is wire format.
function prefKey(m: Model, id: number): number {
  const suit = Math.floor(id / 13);
  const trump = suit === m.powerSuit ? 1 : 0;
  return trump * 4096 + idValue(id) * 4 + suit;
}
function prefSorted(m: Model, ids: number[]): number[] {
  return ids.slice().sort((a, b) => prefKey(m, a) - prefKey(m, b) || a - b);
}
function sortedKnown(m: Model, s: number): number[] {
  return prefSorted(m, Array.from(m.known[s]));
}
function unseenMatching(m: Model, pred: (id: number) => boolean): number[] {
  const out: number[] = [];
  for (let id = 0; id < 52; id++) {
    if (m.unseen.has(id) && pred(id)) out.push(id);
  }
  return prefSorted(m, out);
}
function beats(m: Model, defId: number, atkId: number): boolean {
  return canCover(idToCard(atkId), idToCard(defId), m.powerSuit);
}

function emit(
  m: Model,
  log_type: LogType,
  seat: number | null,
  card_pairs: LogCardPair[] = [],
  defender_index: number | null = null,
): void {
  m.out.push({ log_type, seat, card_pairs, defender_index });
}

function checkConservation(m: Model): void {
  let u = 0;
  for (let s = 0; s < m.n; s++) u += m.unknown[s];
  if (m.unseen.size !== m.deckCount + u) {
    throw new Error(
      `replay desync: unseen=${m.unseen.size} deck=${m.deckCount} hidden=${u}`,
    );
  }
}

/* taking a card out of a hand at the moment it is chosen */
function takeKnown(m: Model, seat: number, id: number): void {
  if (!m.known[seat].delete(id)) throw new Error("replay desync: known card");
  m.unseen.delete(id); // no-op (known ⇒ already seen); keep symmetric
}
function takeFresh(m: Model, seat: number, id: number): void {
  if (!m.unseen.delete(id)) throw new Error("replay desync: fresh card");
  if (m.unknown[seat] <= 0) throw new Error("replay desync: hidden count");
  m.unknown[seat]--;
}

/* --------------------------- derived cascades ---------------------------- */

// Port of refillPlayerHandsWithEvents (common_utils.ts). Emits DRAW logs;
// players who end the refill with no cards while the stock is dry go OUT
// silently (the engine adds no PLAYER_OUT log there either).
function refill(m: Model): void {
  if (stockTotal(m) === 0) {
    // engine's early-return branch: seat order, not rotation order
    for (let i = 0; i < m.n; i++) {
      if (handLen(m, i) === 0 && m.status[i]) {
        m.status[i] = false;
        m.eliminationOrder.push(i);
      }
    }
    return;
  }

  const drawFor = (seat: number): void => {
    const drawn: LogCardPair[] = [];
    while (handLen(m, seat) < CARDS_PER_PLAYER) {
      if (m.deckCount > 0) {
        m.deckCount--;
        m.unknown[seat]++;
        drawn.push({ primary: { ...HIDDEN }, target: null });
      } else if (m.flippedHeld) {
        m.flippedHeld = false;
        m.known[seat].add(m.trumpId);
        drawn.push({ primary: idToCard(m.trumpId), target: null });
      } else {
        break;
      }
    }
    if (drawn.length > 0) emit(m, LOG_TYPE.DRAW, seat, drawn);
  };

  // defender draws first when a clean cover emptied their hand
  if (handLen(m, m.defender) === 0) drawFor(m.defender);

  let p = m.firstAttacker;
  const visited = new Set<number>();
  do {
    if (visited.has(p)) break;
    visited.add(p);
    drawFor(p);
    if (handLen(m, p) === 0 && m.status[p]) {
      m.status[p] = false;
      m.eliminationOrder.push(p);
    }
    p = nextIn(m, p);
  } while (p !== m.firstAttacker);
}

function tableCards(m: Model): LogCardPair[] {
  const out: LogCardPair[] = [];
  for (const b of m.battles) {
    out.push({ primary: idToCard(b.attack), target: null });
    if (b.defense !== null)
      out.push({ primary: idToCard(b.defense), target: null });
  }
  return out;
}

// Shared discard+refill+rotation used by the good-transition (good.ts
// executeRoundTransition) — and, with a different rotation, by cover/pickup.
function discardTable(m: Model): void {
  m.discard += m.battles.length * 2; // engine counts 2 per battle (all covered)
  emit(m, LOG_TYPE.DISCARD, null, tableCards(m));
  m.battles = [];
}

/* ------------------------------- the menus ------------------------------- */

type Option =
  | { kind: "cover"; b: number; id: number | null } // id null = fresh
  | { kind: "pass"; id: number | null }
  | { kind: "pickup" }
  | { kind: "attack"; seat: number; id: number | null }
  | { kind: "good"; seat: number };

// The top-level menu: every (actor, first-card-or-action) atom legal in the
// current public state, in a FIXED order (wire format!): for each seat
// ascending — defender: covers (battle asc; known cards in preference order,
// then fresh), pass (preference order, then fresh), pickup; attacker: attacks
// (preference order, then fresh), good. Mirrors validateAttack/Cover/Pass/
// Pickup/Good, deliberately erring permissive (e.g. attack-after-good is
// server-legal, so it stays in the menu even though the stock UI never
// produces it).
interface Menu {
  opts: Option[];
  weights: number[];
}

function buildTopMenu(m: Model): Menu {
  const opts: Option[] = [];
  const weights: number[] = [];
  const add = (o: Option, w: number) => {
    opts.push(o);
    weights.push(Math.max(w, 1));
  };
  const uncovered = m.battles.filter((b) => b.defense === null).length;
  const allCovered = m.battles.length > 0 && uncovered === 0;
  const defHand = handLen(m, m.defender);
  const tableVals = new Set<number>();
  for (const b of m.battles) {
    tableVals.add(idValue(b.attack));
    if (b.defense !== null) tableVals.add(idValue(b.defense));
  }

  for (let seat = 0; seat < m.n; seat++) {
    if (!m.status[seat]) continue;

    if (seat === m.defender) {
      if (m.battles.length === 0) continue;
      // covers (validateCover: any uncovered battle, any beating card)
      for (let b = 0; b < m.battles.length; b++) {
        if (m.battles[b].defense !== null) continue;
        const atk = m.battles[b].attack;
        let pos = 0;
        for (const id of sortedKnown(m, seat)) {
          if (beats(m, id, atk))
            add({ kind: "cover", b, id }, geo(V1.COVER, pos++));
        }
        if (
          m.unknown[seat] > 0 &&
          unseenMatching(m, (id) => beats(m, id, atk)).length > 0
        ) {
          add({ kind: "cover", b, id: null }, V1.COVER_FRESH);
        }
      }
      // pass / perevod (validatePass: nothing covered, one rank on the table,
      // next player must be able to cover everything incl. the passed card)
      if (uncovered === m.battles.length) {
        const v = idValue(m.battles[0].attack);
        const oneRank = m.battles.every((b) => idValue(b.attack) === v);
        const next = nextIn(m, m.defender);
        if (
          oneRank &&
          next !== seat &&
          handLen(m, next) >= m.battles.length + 1
        ) {
          let pos = 0;
          for (const id of sortedKnown(m, seat)) {
            if (idValue(id) === v)
              add({ kind: "pass", id }, geo(V1.PASS, pos++));
          }
          if (
            m.unknown[seat] > 0 &&
            unseenMatching(m, (id) => idValue(id) === v).length > 0
          ) {
            add({ kind: "pass", id: null }, V1.PASS_FRESH);
          }
        }
      }
      // pickup (validatePickup: any non-empty table)
      add({ kind: "pickup" }, V1.PICKUP);
    } else {
      // attacks
      if (m.battles.length === 0) {
        // first attack of the bout: only the first attacker (validateAttack)
        if (seat === m.firstAttacker && defHand >= 1) {
          let pos = 0;
          for (const id of sortedKnown(m, seat))
            add({ kind: "attack", seat, id }, geo(V1.ATTACK, pos++));
          if (m.unknown[seat] > 0 && m.unseen.size > 0)
            add({ kind: "attack", seat, id: null }, V1.ATTACK_FRESH_LEAD);
        }
      } else if (uncovered + 1 <= defHand) {
        let pos = 0;
        for (const id of sortedKnown(m, seat)) {
          if (tableVals.has(idValue(id)))
            add({ kind: "attack", seat, id }, geo(V1.ATTACK, pos++));
        }
        if (
          m.unknown[seat] > 0 &&
          unseenMatching(m, (id) => tableVals.has(idValue(id))).length > 0
        ) {
          add({ kind: "attack", seat, id: null }, V1.ATTACK_FRESH);
        }
      }
      // good (validateGood: not the first attacker on an empty table,
      // not already said)
      if (
        !m.goods.has(seat) &&
        !(m.battles.length === 0 && seat === m.firstAttacker)
      ) {
        add({ kind: "good", seat }, allCovered ? V1.GOOD_COVERED : V1.GOOD_OPEN);
      }
    }
  }
  return { opts, weights };
}

// Continuation menu for multi-card ATTACK / PASS atoms: index 0 is always
// STOP, then known cards ascending, then fresh (id null). Constraints are
// validated against the state at atom START (the server validates the whole
// card array up front), with `count` cards already chosen.
interface ContOption {
  id: number | null; // null = fresh; STOP is handled as index 0 separately
}

function buildAttackCont(
  m: Model,
  seat: number,
  firstAttack: boolean,
  v0: number,
  tableVals: Set<number>,
  uncoveredBefore: number,
  count: number,
): ContOption[] {
  const opts: ContOption[] = [];
  if (uncoveredBefore + count + 1 > handLen(m, m.defender)) return opts;
  const ok = (id: number) =>
    firstAttack ? idValue(id) === v0 : tableVals.has(idValue(id));
  for (const id of sortedKnown(m, seat)) if (ok(id)) opts.push({ id });
  if (m.unknown[seat] > 0 && unseenMatching(m, ok).length > 0)
    opts.push({ id: null });
  return opts;
}

function buildPassCont(
  m: Model,
  seat: number,
  v0: number,
  nextSeat: number,
  battlesBefore: number,
  count: number,
): ContOption[] {
  const opts: ContOption[] = [];
  if (battlesBefore + count + 1 > handLen(m, nextSeat)) return opts;
  for (const id of sortedKnown(m, seat))
    if (idValue(id) === v0) opts.push({ id });
  if (
    m.unknown[seat] > 0 &&
    unseenMatching(m, (id) => idValue(id) === v0).length > 0
  )
    opts.push({ id: null });
  return opts;
}

/* ------------------------- applying chosen atoms -------------------------- */
// Each apply* mirrors the corresponding execute* in the server actions,
// including the exact order of emitted logs and rotation updates.

function applyAttack(m: Model, seat: number, ids: number[]): void {
  for (const id of ids) m.battles.push({ attack: id, defense: null });
  emit(
    m,
    LOG_TYPE.ATTACK,
    seat,
    ids.map((id) => ({ primary: idToCard(id), target: null })),
  );
  m.goods.clear();
  if (handLen(m, seat) === 0) {
    // executeAttack: out immediately, even with stock remaining
    m.status[seat] = false;
    m.eliminationOrder.push(seat);
    emit(m, LOG_TYPE.PLAYER_OUT, seat);
  }
}

function applyCover(m: Model, b: number, coverId: number): void {
  m.battles[b].defense = coverId;
  emit(m, LOG_TYPE.COVER, m.defender, [
    { primary: idToCard(coverId), target: idToCard(m.battles[b].attack) },
  ]);

  if (handLen(m, m.defender) === 0) {
    // executeCover's clean-sweep branch: discard, refill (defender first),
    // defender leads next bout — or goes out if the stock is dry.
    discardTable(m);
    refill(m);
    m.firstAttacker = m.defender;
    m.goods.clear();
    if (handLen(m, m.defender) === 0) {
      const wasIn = m.status[m.firstAttacker];
      m.status[m.firstAttacker] = false;
      if (wasIn) m.eliminationOrder.push(m.firstAttacker);
      emit(m, LOG_TYPE.PLAYER_OUT, m.firstAttacker); // engine logs even if refill already marked OUT
      m.firstAttacker = nextIn(m, m.firstAttacker);
    }
    m.defender = nextIn(m, m.firstAttacker);
    emit(m, LOG_TYPE.DEFENDER_CHANGE, null, [], m.defender);
  } else {
    m.goods.clear(); // every cover lets attackers reconsider
  }
}

function applyPass(m: Model, seat: number, ids: number[]): void {
  for (const id of ids) m.battles.push({ attack: id, defense: null });
  emit(
    m,
    LOG_TYPE.PASS,
    seat,
    ids.map((id) => ({ primary: idToCard(id), target: null })),
  );
  m.goods.clear();
  const next = nextIn(m, m.defender);
  if (stockTotal(m) === 0 && handLen(m, seat) === 0) {
    m.status[seat] = false;
    m.eliminationOrder.push(seat);
    emit(m, LOG_TYPE.PLAYER_OUT, seat);
  }
  m.defender = next;
  emit(m, LOG_TYPE.DEFENDER_CHANGE, null, [], m.defender);
}

function applyPickup(m: Model): void {
  const cards = tableCards(m);
  emit(m, LOG_TYPE.PICKUP, m.defender, cards);
  for (const pair of cards) m.known[m.defender].add(cardId(pair.primary));
  m.battles = [];
  refill(m);
  m.firstAttacker = nextIn(m, m.defender);
  m.defender = nextIn(m, m.firstAttacker);
  emit(m, LOG_TYPE.DEFENDER_CHANGE, null, [], m.defender);
  m.goods.clear();
}

function applyGood(m: Model, seat: number): void {
  emit(m, LOG_TYPE.GOOD, seat);
  m.goods.add(seat);
  const attackers: number[] = [];
  for (let s = 0; s < m.n; s++)
    if (s !== m.defender && m.status[s]) attackers.push(s);
  const allGood =
    attackers.length > 0 && attackers.every((s) => m.goods.has(s));
  const allCovered =
    m.battles.length > 0 && m.battles.every((b) => b.defense !== null);
  if (allGood && allCovered) {
    // executeRoundTransition
    discardTable(m);
    refill(m);
    m.firstAttacker = m.defender;
    m.defender = nextIn(m, m.firstAttacker);
    emit(m, LOG_TYPE.DEFENDER_CHANGE, null, [], m.defender);
    m.goods.clear();
  }
}

/* --------------------------- the shared driver --------------------------- */
// One function runs both directions. In encode mode `source` supplies the
// actual game (the info-bearing logs); in decode mode choices come back out
// of the integer. This symmetry is the round-trip guarantee.

export interface InfoSource {
  peek(): ReplayLogEntry; // next info log (throws if exhausted)
  advance(): void;
  exhausted(): boolean;
  seatOf(pid: string | null): number;
}

export const INFO_TYPES: LogType[] = [
  LOG_TYPE.ATTACK,
  LOG_TYPE.COVER,
  LOG_TYPE.PASS,
  LOG_TYPE.PICKUP,
  LOG_TYPE.GOOD,
];

// Identity of a freshly revealed card. The feasible unseen cards are listed
// in preference order; weight C(U-1-j, u-1) is the (unnormalized) chance that
// the best of the player's u hidden cards (out of U unseen) sits at position
// j — the "players play their cheapest legal card" model from the C
// reference. Quantized to ID_QUANT, clamped to >= 1 so every feasible card
// stays encodable. Uniform fallback when u < 2 (model degenerates).
function codeFreshIdentity(
  m: Model,
  coder: Coder,
  seat: number,
  pred: (id: number) => boolean,
  actualId?: number,
): number {
  const feasible = unseenMatching(m, pred);
  if (feasible.length === 0) throw new Error("replay desync: no fresh card");
  let chosen: number | undefined;
  if (actualId !== undefined) {
    chosen = feasible.indexOf(actualId);
    if (chosen < 0) throw new Error("replay desync: fresh card not feasible");
  }
  const U = m.unseen.size;
  const u = m.unknown[seat];
  let weights: number[];
  if (u >= 2) {
    const scale = Math.floor(comb(U - 1, u - 1) / V1.ID_QUANT) + 1;
    weights = feasible.map((_, j) => {
      const w = U - 1 - j >= u - 1 ? comb(U - 1 - j, u - 1) : 0;
      return Math.max(Math.floor(w / scale), 1);
    });
  } else {
    weights = feasible.map(() => 1);
  }
  const k = coder.code(weights, chosen);
  return feasible[k];
}

function findTopIndex(
  m: Model,
  opts: Option[],
  log: ReplayLogEntry,
  src: InfoSource,
): number {
  const match = (o: Option): boolean => {
    switch (log.log_type) {
      case LOG_TYPE.ATTACK: {
        if (o.kind !== "attack") return false;
        if (o.seat !== src.seatOf(log.player_id)) return false;
        const id0 = cardId(log.card_pairs[0].primary);
        return m.known[o.seat].has(id0) ? o.id === id0 : o.id === null;
      }
      case LOG_TYPE.COVER: {
        if (o.kind !== "cover") return false;
        const target = cardId(log.card_pairs[0].target!);
        // executeCover targets the first uncovered battle holding this card
        let bIdx = -1;
        for (let b = 0; b < m.battles.length; b++) {
          if (m.battles[b].defense === null && m.battles[b].attack === target) {
            bIdx = b;
            break;
          }
        }
        if (o.b !== bIdx) return false;
        const cov = cardId(log.card_pairs[0].primary);
        return m.known[m.defender].has(cov) ? o.id === cov : o.id === null;
      }
      case LOG_TYPE.PASS: {
        if (o.kind !== "pass") return false;
        const id0 = cardId(log.card_pairs[0].primary);
        return m.known[m.defender].has(id0) ? o.id === id0 : o.id === null;
      }
      case LOG_TYPE.PICKUP:
        return o.kind === "pickup";
      case LOG_TYPE.GOOD:
        return o.kind === "good" && o.seat === src.seatOf(log.player_id);
      default:
        return false;
    }
  };
  const idx = opts.findIndex(match);
  if (idx < 0) {
    throw new Error(
      `replay desync: logged ${log.log_type} not in menu of ${opts.length}`,
    );
  }
  return idx;
}

export function runReplay(
  coder: Coder,
  n: number,
  trumpId: number,
  firstAttacker: number,
  source: InfoSource | null,
): Model {
  const deckSize = n > 4 ? 52 : 36;
  const lowest = deckSize === 52 ? 1 : 5;

  const m: Model = {
    n,
    trumpId,
    powerSuit: Math.floor(trumpId / 13),
    status: new Array(n).fill(true),
    known: Array.from({ length: n }, () => new Set<number>()),
    unknown: new Array(n).fill(CARDS_PER_PLAYER),
    unseen: new Set<number>(),
    deckCount: deckSize - n * CARDS_PER_PLAYER - 1,
    flippedHeld: true,
    battles: [],
    firstAttacker,
    defender: (firstAttacker + 1) % n, // set_positions()
    goods: new Set<number>(),
    eliminationOrder: [],
    discard: 0,
    out: [],
  };
  for (let suit = 0; suit < 4; suit++) {
    for (let v = lowest; v <= ACE_VALUE; v++) {
      const id = suit * 13 + (v - 1);
      if (id !== trumpId) m.unseen.add(id);
    }
  }
  emit(m, LOG_TYPE.GAME_START, null);

  let atoms = 0;
  while (inCount(m) > 1) {
    if (++atoms > MAX_ATOMS) throw new Error("replay guard: too many events");
    checkConservation(m);

    const { opts, weights } = buildTopMenu(m);
    if (opts.length === 0) throw new Error("replay desync: no legal moves");

    let log: ReplayLogEntry | null = null;
    let chosen: number | undefined;
    if (source) {
      if (source.exhausted())
        throw new Error("incomplete game: logs ended before the fool was known");
      log = source.peek();
      chosen = findTopIndex(m, opts, log, source);
    }
    const opt = opts[coder.code(weights, chosen)];

    switch (opt.kind) {
      case "attack": {
        const seat = opt.seat;
        const firstAttack = m.battles.length === 0;
        const uncoveredBefore = m.battles.filter(
          (b) => b.defense === null,
        ).length;
        const tableVals = new Set<number>();
        for (const b of m.battles) {
          tableVals.add(idValue(b.attack));
          if (b.defense !== null) tableVals.add(idValue(b.defense));
        }
        const ids: number[] = [];
        // first card
        let id0: number;
        if (opt.id !== null) {
          id0 = opt.id;
          takeKnown(m, seat, id0);
        } else {
          const pred = firstAttack
            ? () => true
            : (id: number) => tableVals.has(idValue(id));
          id0 = codeFreshIdentity(
            m,
            coder,
            seat,
            pred,
            log ? cardId(log.card_pairs[0].primary) : undefined,
          );
          takeFresh(m, seat, id0);
        }
        ids.push(id0);
        const v0 = idValue(id0);
        // continuation: 0=stop, then more cards (same rank on a first attack,
        // any table rank otherwise)
        for (;;) {
          const cont = buildAttackCont(
            m,
            seat,
            firstAttack,
            v0,
            tableVals,
            uncoveredBefore,
            ids.length,
          );
          let contChosen: number | undefined;
          if (log) {
            if (ids.length < log.card_pairs.length) {
              const next = cardId(log.card_pairs[ids.length].primary);
              const wantKnown = m.known[seat].has(next);
              contChosen =
                1 +
                cont.findIndex((c) =>
                  wantKnown ? c.id === next : c.id === null,
                );
              if (contChosen === 0)
                throw new Error("replay desync: attack continuation");
            } else {
              contChosen = 0;
            }
          }
          const k = coder.code([V1.STOP, ...cont.map(() => 1)], contChosen);
          if (k === 0) break;
          const pick = cont[k - 1];
          let id: number;
          if (pick.id !== null) {
            id = pick.id;
            takeKnown(m, seat, id);
          } else {
            const pred = firstAttack
              ? (cid: number) => idValue(cid) === v0
              : (cid: number) => tableVals.has(idValue(cid));
            id = codeFreshIdentity(
              m,
              coder,
              seat,
              pred,
              log ? cardId(log.card_pairs[ids.length].primary) : undefined,
            );
            takeFresh(m, seat, id);
          }
          ids.push(id);
        }
        applyAttack(m, seat, ids);
        break;
      }

      case "cover": {
        const atk = m.battles[opt.b].attack;
        let coverId: number;
        if (opt.id !== null) {
          coverId = opt.id;
          takeKnown(m, m.defender, coverId);
        } else {
          coverId = codeFreshIdentity(
            m,
            coder,
            m.defender,
            (id) => beats(m, id, atk),
            log ? cardId(log.card_pairs[0].primary) : undefined,
          );
          takeFresh(m, m.defender, coverId);
        }
        applyCover(m, opt.b, coverId);
        break;
      }

      case "pass": {
        const seat = m.defender;
        const v0 = idValue(m.battles[0].attack);
        const battlesBefore = m.battles.length;
        const nextSeat = nextIn(m, m.defender);
        const ids: number[] = [];
        let id0: number;
        if (opt.id !== null) {
          id0 = opt.id;
          takeKnown(m, seat, id0);
        } else {
          id0 = codeFreshIdentity(
            m,
            coder,
            seat,
            (id) => idValue(id) === v0,
            log ? cardId(log.card_pairs[0].primary) : undefined,
          );
          takeFresh(m, seat, id0);
        }
        ids.push(id0);
        for (;;) {
          const cont = buildPassCont(
            m,
            seat,
            v0,
            nextSeat,
            battlesBefore,
            ids.length,
          );
          let contChosen: number | undefined;
          if (log) {
            if (ids.length < log.card_pairs.length) {
              const next = cardId(log.card_pairs[ids.length].primary);
              const wantKnown = m.known[seat].has(next);
              contChosen =
                1 +
                cont.findIndex((c) =>
                  wantKnown ? c.id === next : c.id === null,
                );
              if (contChosen === 0)
                throw new Error("replay desync: pass continuation");
            } else {
              contChosen = 0;
            }
          }
          const k = coder.code([V1.STOP, ...cont.map(() => 1)], contChosen);
          if (k === 0) break;
          const pick = cont[k - 1];
          let id: number;
          if (pick.id !== null) {
            id = pick.id;
            takeKnown(m, seat, id);
          } else {
            id = codeFreshIdentity(
              m,
              coder,
              seat,
              (cid) => idValue(cid) === v0,
              log ? cardId(log.card_pairs[ids.length].primary) : undefined,
            );
            takeFresh(m, seat, id);
          }
          ids.push(id);
        }
        applyPass(m, seat, ids);
        break;
      }

      case "pickup":
        applyPickup(m);
        break;

      case "good":
        applyGood(m, opt.seat);
        break;
    }

    if (source) source.advance();
  }

  if (source && !source.exhausted()) {
    throw new Error("replay desync: logs continue after the game ended");
  }
  return m;
}

/* ------------------------- header & shared exports ------------------------ */

export function trumpAlphabet(n: number): number[] {
  const lowest = n > 4 ? 1 : 5;
  const out: number[] = [];
  for (let suit = 0; suit < 4; suit++) {
    for (let v = lowest; v < ACE_VALUE; v++) out.push(suit * 13 + (v - 1));
  }
  return out; // the flipped card is redrawn while it is an ace (start_game)
}
