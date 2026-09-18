// Decode a replay-share URL into the flat JSON the og_explain pipeline consumes.
// The kernel decodes it (e2e/helpers/replay_decode.ts: the code's summary and
// replay_decode's log stream through the generated readers). Emits {logs,
// trumpCard, powerSuit, firstAttacker, fool, eliminationOrder, playerCount}.
//
//   node --import tsx c/tools/og_explain/decode_to_json.mjs "<replay-url>" replay_decoded.json
//
// The out-path is an explicit argument (not stdout) because instantiating the
// wasm engine prints a "[perf] ..." line to stdout that would corrupt piped JSON.
import fs from 'node:fs';
import { ensureBotsAsync } from '../../../sdk/ts/wasm/bots.ts';
import { decodeReplayLink } from '../../../e2e/helpers/replay_decode.ts';
import * as G from '../../../sdk/ts/gen/game_layout.bots.ts';

const url = process.argv[2];
const outPath = process.argv[3];
if (!url || !outPath) { console.error('usage: decode_to_json.mjs <replay-url> <out.json>'); process.exit(2); }

await ensureBotsAsync();
const d = decodeReplayLink(url);
const LOG = {};
for (const [k, v] of Object.entries(G)) if (k.startsWith('LOG_')) LOG[v] = k.slice(4).toLowerCase();

const seatOf = (s) => (s == null || s < 0) ? null : s;
const card = (c) => (c == null || c.suit < 0) ? { suit: -1, value: -1 } : { suit: c.suit, value: c.value };
const logs = d.logs.map((l) => ({
  t: LOG[l.type],
  seat: seatOf(l.seat),
  def: l.defender < 0 ? null : l.defender,
  cards: l.pairs.map((cp) => ({
    p: card(cp.primary),
    tg: cp.target ? card(cp.target) : null,
  })),
}));

fs.writeFileSync(outPath, JSON.stringify({
  logs,
  trumpCard: card(d.summary.trump),
  powerSuit: d.summary.powerSuit,
  firstAttacker: d.summary.firstAttacker,
  fool: d.summary.fool,
  eliminationOrder: [...d.summary.elimination],
  playerCount: d.summary.numPlayers,
}));
console.error(`decoded ${logs.length} logs -> ${outPath}`);
