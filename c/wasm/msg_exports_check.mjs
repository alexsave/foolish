// msg.wasm's export surface, asserted.
//
// The module is encode-only on the replay side: it writes a share link at the
// end of a game and never reads one. That is enforced by OMISSION - the
// Makefile's export list simply does not name the decoders, and wasm-ld drops
// what nothing exports or calls. An omission is a silent thing: paste a decode
// export back in while copying a list around (which is how json_out.c got into
// this target in the first place) and the module quietly grows a reader again,
// with nothing to say so.
//
// So the omission is checked here, in both directions - no decoder came back,
// and no encoder went missing - because a filter typo that dropped the ENCODER
// would also be silent, and would be the worse bug.
//
// Usage: node wasm/msg_exports_check.mjs build/msg.wasm

import { readFileSync } from 'node:fs';

const path = process.argv[2];
if (!path) { console.error('usage: msg_exports_check.mjs <msg.wasm>'); process.exit(2); }

const exports = WebAssembly.Module.exports(new WebAssembly.Module(readFileSync(path)))
    .filter((e) => e.kind === 'function')
    .map((e) => e.name);
const has = (n) => exports.includes(n);

// The share link, end to end: game -> code -> base32 -> URL, plus the extras
// blob (nicknames, move timings) that rides behind the dash.
const MUST_HAVE = [
    'wasm_replay_encode_v6',
    'wasm_replay_extras_encode',
    'wasm_replay_b32_encode',
    'wasm_replay_link',
    'wasm_replay_error_detail',
];

// Reading a code back. `wasm_msg_decode` is deliberately NOT here: the FMSG
// envelope is the extension's own inbound format, and its body decode
// (replay_decode_atoms_v6) is what opening a bubble IS.
const MUST_NOT_HAVE = [
    'wasm_replay_decode',
    'wasm_replay_b32_decode',
    'wasm_replay_extras_decode',
    'wasm_replay_link_parse',
];

const missing = MUST_HAVE.filter((n) => !has(n));
const present = MUST_NOT_HAVE.filter((n) => has(n));

if (missing.length || present.length) {
    if (missing.length) {
        console.error(`msg.wasm is missing the encoders it exists to carry: ${missing.join(', ')}`);
        console.error('  -> an export was filtered out or renamed; the share link is broken.');
    }
    if (present.length) {
        console.error(`msg.wasm exports replay DECODERS: ${present.join(', ')}`);
        console.error('  -> the extension writes share links and never reads them. Drop the export');
        console.error('     from WASM_MSG_EXPORTS / WASM_MSG_API_EXPORTS, or change this list and say why.');
    }
    process.exit(1);
}

console.log(`msg.wasm exports ok (${exports.length} functions; ${MUST_HAVE.length} encoders present, ${MUST_NOT_HAVE.length} decoders absent)`);
