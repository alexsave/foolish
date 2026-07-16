// KERNEL-PATH 4v4 generator. Self-plays a seeded 4-octogen (0-3) vs 4-random
// (4-7) game driving EVERY seat through wasmChooseMoveDirect + belief_log_bytes
// — the deployed server's exact decision path. Emits a self-contained record
// {seed, url, moves (EXACT recorded picks), rd (board logs + metadata)} for the
// first game an octogen wins. The X-ray then replays the EXACT moves (not the
// lossy replay URL), so octogen's belief reproduces byte-for-byte and every
// pick matches — a true "what the wasm bot actually thought" view.
//
//   OGX_GEN_OUT=<out.json> [GEN_MAX=150] [GEN_BASE=101] SC_RUN=1 \
//     TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx --test e2e/_wasm_4v4_gen.test.ts
import { test } from 'node:test';
import { writeFileSync } from 'node:fs';
import { start_game_packed } from '../server/api/common/game_lifecycle.ts';
import { runPackedGameAction, applyKernelStateToGame, __setDealSeedOverride } from '../sdk/ts/wasm/engine.ts';
import { encodeAction } from '../sdk/ts/wire/awire.ts';
import { logsFromKernelExport, decodeLogs } from '../sdk/ts/wire/logwire.ts';
import { wasmChooseMoveDirect, __ensureBots, STRAT } from '../sdk/ts/wasm/bots.ts';
import { shouldBotActCore } from '../server/api/common/pure_bot_actions.ts';
import { kernelReplayEncodeV6FromGame } from '../sdk/ts/wasm/bots.ts';
import { base32Encode, bytesToBigint, gameToUrl } from '../server/api/common/replay/codec.ts';
import { encodeExtras, joinReplayCode, moveTimesFromLogs } from '../server/api/common/replay/extras.ts';
import { PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY } from '../server/api/core/types.ts';

__ensureBots();
const NP = 8;
// Which seats are octogen (default 0-3 = 4v4); OGX_OCTO_SEATS=0,1,2,3,4,5,6,7 for
// an all-octogen table. The rest are random.
const OCTO = new Set((process.env.OGX_OCTO_SEATS || '0,1,2,3').split(',').filter((s) => s !== '').map(Number));
const env = { CD_BUDGET: 'prod', CD_RACE: '1', CD_RACE_C: '75' };
const cat = (a: Uint8Array, b: Uint8Array) => { const m = new Uint8Array(a.length + b.length); m.set(a); m.set(b, a.length); return m; };
const stratOf = (s: number) => OCTO.has(s) ? STRAT.octogen : STRAT.random;
let ogN = 0, rnN = 0;
const names = Array.from({ length: NP }, (_, i) => OCTO.has(i) ? `%OCTOGEN ${++ogN}` : `%RANDOM ${++rnN}`);
const mkGame = () => ({
    players: Array.from({ length: NP }, (_, i) => ({ player_id: `p${i}`, name: names[i], status: PLAYER_STATUS.READY, is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: OCTO.has(i) ? (STRATEGY_KEY as any).OCTOGEN : (STRATEGY_KEY as any).RANDOM })),
    deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.WAITING, deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [], good_timestamp: null, good_players: [], game_seed: null,
}) as any;
const mkMove = (p: any) => { const m: any = { kind: p.type }; if (p.type !== 'pickup' && p.type !== 'good') m.cards = p.cards; if (p.type === 'cover') m.attack_cards = p.attack_cards; return m; };
const seedBytes = (n: number) => { const b = new Uint8Array(32); for (let i = 0; i < 32; i++) b[i] = (n * 2654435761 + i * 40503 + 7) & 0xff; return b; };
const drive = (g: any, seat: number, move: any) => { let m = 0; g.players.forEach((p: any, k: number) => { if (p.is_ai) m |= 1 << k; }); return runPackedGameAction(g, seat, encodeAction(move), m, []) as any; };
const seatOfId = (id: string | null) => (id && id[0] === 'p') ? Number(id.slice(1)) : -1;

const playOne = (attempt: number): any | null => {
    let rs = (attempt * 2654435761 + 12345) >>> 0;
    const rnd = () => { rs = (rs * 1664525 + 1013904223) >>> 0; return rs / 4294967296; };
    __setDealSeedOverride(seedBytes(attempt));
    const g = mkGame();
    const startRun: any = start_game_packed(g);
    const origFlip = g.flipped ? { suit: g.flipped.suit, value: g.flipped.value } : null;
    let belief = new Uint8Array(0);
    const gameLogs: any[] = [];
    const moves: any[] = [];
    let ts = 1;
    const absorb = (wireIn: Uint8Array) => {
        if (!wireIn || wireIn.length <= 2) return;
        const w = logsFromKernelExport(wireIn, ts); ts += 700 + (ts % 137);
        belief = cat(belief, w);
        for (const l of decodeLogs(w, g.id, g.players)) gameLogs.push(l);
    };
    absorb(startRun.logsWire);
    let guard = 0;
    while (g.status !== GAME_STATUS.GAME_OVER && guard++ < 8000) {
        const elig: number[] = [];
        for (let i = 0; i < NP; i++) if (shouldBotActCore(g as never, g.players[i] as never, i)) elig.push(i);
        if (!elig.length) break;
        for (let i = elig.length - 1; i > 0; i--) { const j = Math.floor(rnd() * (i + 1)); [elig[i], elig[j]] = [elig[j], elig[i]]; }
        let acted = false;
        for (const pi of elig) {
            if (belief.length) g.belief_log_bytes = belief; else delete g.belief_log_bytes;
            const q = wasmChooseMoveDirect(g, `p${pi}`, stratOf(pi), { env });
            if (!q) continue;
            const run = drive(g, pi, mkMove(q));
            if (!run?.ok) continue;
            moves.push({ seat: pi, type: q.type, cards: q.cards || undefined, attack_cards: q.attack_cards || undefined });
            absorb(run.logsWire);
            applyKernelStateToGame(g, run.post, `p${pi}`);
            acted = true; break;
        }
        if (!acted) break;
    }
    if (g.status !== GAME_STATUS.GAME_OVER) return null;
    return { g, gameLogs, moves, origFlip };
};

// GameLog[] -> the decoded-replay shape multi_page.py consumes (t/seat/cards/def).
const toRd = (logs: any[], g: any, origFlip: any) => ({
    playerCount: NP, powerSuit: g.power_suit, trumpCard: origFlip,
    firstAttacker: g.first_attacker,
    fool: g.players.findIndex((p: any) => !g.elimination_order.includes(p.player_id)),
    eliminationOrder: g.elimination_order.map((id: string) => seatOfId(id)),
    logs: logs.map((l: any) => ({
        t: l.log_type, seat: seatOfId(l.player_id),
        cards: (l.card_pairs || []).map((pr: any) => ({ p: pr.primary, tg: pr.target ?? null })),
        def: l.defender_index,
    })),
});

test('generate a kernel-path 4v4 octogen-win record', { skip: !process.env.OGX_GEN_OUT }, async () => {
    const MAX = Number(process.env.GEN_MAX || 150);
    const BASE = Number(process.env.GEN_BASE || 101);
    for (let a = 0; a < MAX; a++) {
        const r = playOne(BASE + a);
        if (!r) continue;
        const { g, gameLogs, moves, origFlip } = r;
        const winnerSeat = seatOfId(g.elimination_order[0]);
        if (!OCTO.has(winnerSeat)) continue;
        let url = '';
        try {
            // v6 from the game + its seed — the one producer (v5 is gone).
            const hx = (h: string) => Uint8Array.from(h.match(/../g)!.map((b) => parseInt(b, 16)));
            const bytes = kernelReplayEncodeV6FromGame(g as never, hx((g as any).game_seed));
            const encoded = { bytes, x: bytesToBigint(bytes), byteLength: bytes.length,
                base32: base32Encode(bytes), url: gameToUrl(bytesToBigint(bytes)) };
            const extras = encodeExtras(g.players.map((p: any) => p.name), moveTimesFromLogs(gameLogs as never));
            url = `WWW.FOOLISH.CARDS/${joinReplayCode(encoded.base32, extras)}`;
        } catch (e: any) { process.stderr.write(`attempt ${a}: url encode failed: ${e.message}\n`); }
        const rd = toRd(gameLogs, g, origFlip);
        const out = { seed: g.game_seed, url, octogenSeats: [...OCTO], playerCount: NP, moves, rd };
        writeFileSync(process.env.OGX_GEN_OUT!, JSON.stringify(out));
        process.stderr.write(`GEN: seat ${winnerSeat} won; ${moves.length} moves, ${gameLogs.length} logs, url=${url ? 'ok' : 'FAILED'} elim=${JSON.stringify(rd.eliminationOrder)} fool=${rd.fool} -> ${process.env.OGX_GEN_OUT}\n`);
        return;
    }
    process.stderr.write('GEN: no octogen win found in budget\n');
});
