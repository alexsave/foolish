/* =============================================================================
 * Derived ids: a UUID shape with no entropy behind it
 * =============================================================================
 * The product's invariant is that exactly one draw in the system is truly
 * random - the crypto deal seed, once per live game (injectDealSeed in
 * sdk/ts/wasm/engine.ts). Everything downstream of it must be a function of its
 * inputs, so the same inputs replay to the same bytes.
 *
 * `crypto.randomUUID()` broke that in two quiet places. A session log row got a
 * random `id` when it was appended, and another random `id` when the packed
 * wire was decoded back into rows - so `unpackLogs(bytes)` was not a pure
 * function of `bytes`, and replaying one game twice produced two states that
 * deep-compare unequal for a reason that has nothing to do with the game.
 * Neither id is ever read for a decision (server/api/common/common_utils.ts
 * clones it and nothing else looks at it), so nothing needed the entropy - it
 * was only ever "give me a distinct string".
 *
 * derivedUuid gives the distinct string without the entropy:
 *
 *   namespace   what the ids belong to (a game id, a test file) - hashed
 *   seq         which one it is inside that namespace - carried verbatim
 *
 * Uniqueness inside a namespace is exact rather than probabilistic, because
 * `seq` lands in the low 48 bits unhashed. Across namespaces it rests on a
 * 64-bit hash of the namespace string, which is the same bet a random v4 makes
 * and a far smaller population.
 *
 * `seq` is ALSO hashed into the leading nibbles, so consecutive ids differ from
 * their first character. That matters because callers slice these: the e2e
 * harness builds a game id as `m${uuid().slice(0, 5)}`, and an id whose prefix
 * was a pure function of the namespace would hand every game in a file the same
 * name.
 *
 * The output is a well-formed RFC 4122 v4 string (version nibble 4, variant
 * bits 10) because it goes into Postgres `uuid` columns, which validate it.
 * It is deliberately NOT a v5/name-based UUID: nothing consuming these ids
 * cares about the namespace algorithm, and a real v5 needs SHA-1.
 *
 * Not for anything security-relevant. A derived id is guessable by design; a
 * token, an invite code or a session nonce must keep drawing from crypto.
 * ========================================================================== */

/* FNV-1a over two lanes with different offset bases, giving 64 bits of hash
 * from one pass. Two lanes rather than one because 32 bits collide at ~65k
 * namespaces, which a long test run could plausibly reach. */
function hash64(s: string): [number, number] {
    let a = 0x811c9dc5 >>> 0;
    let b = 0x01000193 >>> 0;
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        a = Math.imul(a ^ c, 16777619) >>> 0;
        b = Math.imul(b ^ (c + i), 2246822519) >>> 0;
        b = ((b << 13) | (b >>> 19)) >>> 0;
    }
    // One avalanche pass each, so a one-character namespace change moves every
    // nibble rather than only the tail.
    a = Math.imul(a ^ (a >>> 16), 2246822507) >>> 0;
    a = Math.imul(a ^ (a >>> 13), 3266489909) >>> 0;
    a = (a ^ (a >>> 16)) >>> 0;
    b = Math.imul(b ^ (b >>> 16), 3266489909) >>> 0;
    b = Math.imul(b ^ (b >>> 13), 2246822507) >>> 0;
    b = (b ^ (b >>> 16)) >>> 0;
    return [a, b];
}

const hex = (n: number, width: number): string => (n >>> 0).toString(16).padStart(width, '0').slice(-width);

/**
 * A UUID-shaped id that is a pure function of (`namespace`, `seq`).
 *
 * @param namespace what the ids belong to - a game id, a test file path.
 * @param seq       0-based index within the namespace; must fit in 48 bits.
 */
export function derivedUuid(namespace: string, seq: number): string {
    if (!Number.isInteger(seq) || seq < 0 || seq > 0xffffffffffff) {
        throw new RangeError(`derivedUuid: seq out of range: ${seq}`);
    }
    const [a, b] = hash64(`${namespace}#${seq}`);
    // 8-4-4-4-12. The version nibble is a literal 4 and the variant nibble is
    // masked to 0b10xx, so Postgres and every UUID parser accept it.
    const variant = ((b >>> 24) & 0x3f) | 0x80;
    return [
        hex(a, 8),
        hex(b >>> 16, 4),
        `4${hex(b & 0x0fff, 3)}`,
        `${hex(variant, 2)}${hex((a >>> 16) & 0xff, 2)}`,
        // 48 bits of sequence, verbatim: exact uniqueness inside the namespace.
        `${hex(Math.floor(seq / 0x100000000), 4)}${hex(seq % 0x100000000, 8)}`,
    ].join('-');
}

/** A counter bound to one namespace, for callers that just want "the next one". */
export function derivedUuidSeq(namespace: string, start = 0): () => string {
    let n = start;
    return () => derivedUuid(namespace, n++);
}
