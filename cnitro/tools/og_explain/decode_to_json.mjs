// Decode a replay-share URL into the flat JSON the og_explain pipeline consumes.
// Pure re-use of the deployed replay codec (no second copy): urlToGame parses
// the base32 code, decodeReplay drives the wasm engine to recover the public
// log stream + game meta. Emits {logs, trumpCard, powerSuit, firstAttacker,
// fool, eliminationOrder, playerCount} on stdout.
//
//   node cnitro/tools/og_explain/decode_to_json.mjs "<replay-url>" replay_decoded.json
//
// The out-path is an explicit argument (not stdout) because instantiating the
// wasm engine prints a "[perf] …" line to stdout that would corrupt piped JSON.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '../../..');
const { urlToGame } = await import(`${ROOT}/supabase/functions/_shared/replay/codec.ts`);
const { decodeReplay } = await import(`${ROOT}/supabase/functions/_shared/replay/decode.ts`);

const url = process.argv[2];
const outPath = process.argv[3];
if (!url || !outPath) { console.error('usage: decode_to_json.mjs <replay-url> <out.json>'); process.exit(2); }

const d = await decodeReplay(urlToGame(url));

const seatOf = (s) => (s == null || s < 0) ? null : s;
const card = (c) => (c == null || c.suit < 0) ? { suit: -1, value: -1 } : { suit: c.suit, value: c.value };
const logs = d.logs.map((l) => ({
  t: l.log_type,
  seat: seatOf(l.seat),
  def: l.defender_index,
  cards: (l.card_pairs || []).map((cp) => ({
    p: card(cp.primary),
    tg: cp.target ? card(cp.target) : null,
  })),
}));

fs.writeFileSync(outPath, JSON.stringify({
  logs,
  trumpCard: card(d.trumpCard),
  powerSuit: d.powerSuit,
  firstAttacker: d.firstAttacker,
  fool: d.fool,
  eliminationOrder: d.eliminationOrder,
  playerCount: d.playerCount,
}));
console.error(`decoded ${logs.length} logs -> ${outPath}`);
