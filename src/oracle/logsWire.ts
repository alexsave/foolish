/* =============================================================================
 * Infinite Oracle — SeatLog[] -> kernel import-logs wire bytes (§8.3)
 * Mirrors importLogs (bots.ts) but from the decoded SeatLog[] (seat already
 * numeric). Wire (wasm_import_logs, wasm_bots_api.c): u16 LE count, then per
 * record i8 type, i8 seat (0xFF null), i8 defender_index (0xFF null), u8
 * n_pairs (<=64), n_pairs x (u8 primary, u8 target) 1-byte wire cards — hidden
 * {-1,-1} -> 0xFE, absent target -> 0xFF. Truncate at 512 records keep-first
 * (mirrors the live bot cap). Encode ONCE on the main thread; ship to workers.
 * ========================================================================== */

import { SeatLog } from '@api/common/replay/core.ts';
import { LOG_TYPE } from '@api/core/types.ts';
import { __LOG_TYPE_TO_INT, __wireLogCard } from '@sdk/ts/wasm/engine.ts';

const MAX_KERNEL_LOGS = 512;   // MAX_LOGS (game.h)
const MAX_KERNEL_PAIRS = 64;   // MAX_LOG_PAIRS (wasm build)

export function encodeLogsWire(logs: SeatLog[]): Uint8Array {
    const n = Math.min(logs.length, MAX_KERNEL_LOGS);
    const out: number[] = [];
    out.push(n & 0xff, (n >> 8) & 0xff);
    for (let i = 0; i < n; i++) {
        const l = logs[i];
        // DRAW-mask (logwire.ts:17-21): the live bot's belief feed NEVER reveals
        // a drawn card's identity — only that a card moved deck->hand. A Decoded
        // replay's DRAW logs carry the identity when it's known, which is ALWAYS
        // for a Format-6 replay (the codec stores real deal/draws). Passing those
        // through would hand octogen every opponent's hidden cards — perfect-
        // memory becomes clairvoyance. So force every DRAW card to hidden here,
        // exactly as the server's belief feed does. (On a v5 replay draws are
        // already masked, so this is a no-op; on v6 it is load-bearing.)
        const isDraw = l.log_type === LOG_TYPE.DRAW;
        out.push((__LOG_TYPE_TO_INT.get(l.log_type) ?? 0) & 0xff);
        out.push((l.seat ?? -1) & 0xff);
        out.push((l.defender_index ?? -1) & 0xff);
        const pairs = l.card_pairs ?? [];
        const np = Math.min(pairs.length, MAX_KERNEL_PAIRS);
        out.push(np & 0xff);
        for (let j = 0; j < np; j++) {
            const p = pairs[j];
            // missing primary -> hidden card (0xFE), matching importLogs; missing
            // target -> the in-band "no card" (0xFF, via __wireLogCard). DRAW
            // primaries are forced hidden regardless of what the replay stored.
            out.push((isDraw || !p.primary) ? 0xfe : (__wireLogCard(p.primary) & 0xff));
            out.push(__wireLogCard(p.target) & 0xff);
        }
    }
    return Uint8Array.from(out);
}
