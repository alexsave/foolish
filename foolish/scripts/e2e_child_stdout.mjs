/* =============================================================================
 * Keep the test's own output off node:test's wire
 * =============================================================================
 * A node:test child does NOT get a private channel for its report. The parent
 * spawns it with NODE_TEST_CONTEXT=child-v8 and the child's reporter writes
 * v8-serialized messages - <ff 0f> + a 4-byte big-endian length + payload -
 * straight to fd 1. Anything the test itself prints goes to the SAME fd, spliced
 * between those frames, and the parent has to tell the two apart by scanning.
 *
 * It does that badly. internal/test_runner/runner.js skips non-serialized bytes
 * only until the head of its buffer starts with the magic; from there it walks
 * frames back-to-back WITHOUT re-checking the magic, so the first print that
 * lands right behind a frame in one read is decoded as a frame header. Its
 * length comes out of a signed 32-bit shift:
 *
 *     (b[2] << 24 | b[3] << 16 | b[4] << 8 | b[5]) + 6
 *
 * so a third byte >= 0x80 makes it NEGATIVE. Negative passes the "not enough
 * data yet, wait for more" guard, and the parent deserializes garbage:
 *
 *     Unable to deserialize cloned data due to invalid or unsupported version.
 *
 * which surfaces as a whole file failing with no failing assertion in it. Every
 * non-ASCII line qualifies: this suite prints "  §12.2-1: ..." (0x20 0x20 0xc2)
 * and a bot play-by-play of "⚔️ / 🛡️ / 📥" lines. Whether a print shares a read
 * with a frame is purely a question of how long the parent went without reading,
 * which is why it survives one file alone and dies once the runner has several
 * children and a reporter competing for that one event loop.
 *
 * So: fd 1 carries the protocol and nothing else. Everything the test writes
 * goes to fd 2, which the parent forwards verbatim as test:stderr - the
 * reporters print it exactly as they printed it from stdout.
 * ========================================================================== */

// Only in a test child. The lane process itself owns its stdout.
if (process.env.NODE_TEST_CONTEXT) {
    const stdout = process.stdout;
    const write = stdout.write.bind(stdout);
    // A report frame is a Buffer whose first two bytes are the v8 header. The
    // reporter pushes one whole message per write, so the magic is always at 0.
    const isFrame = (c) => typeof c !== 'string' && c?.length >= 2 && c[0] === 0xff && c[1] === 0x0f;
    stdout.write = function (chunk, ...rest) {
        if (isFrame(chunk)) return write(chunk, ...rest);
        return process.stderr.write(chunk, ...rest);
    };
}
