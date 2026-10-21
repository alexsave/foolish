# cbase speedups since the move off Lambda

As of 2026-10-21.
Numbers come from the PR bodies, commit messages and session notes, not from new runs.
Unless marked "nano", they are from the local rig (Docker on the Mac, arm64), where "µs/move" is server CPU per move.

From the first VM build on main (5d5ef505b) to today's main (284f0dc4f), the server went from 181 to 67 µs per move at human pace and from 59.7 to 36.0 µs at saturation.
Two-player throughput rose from 16.7k to 27.7k moves/s, sat2 p99 fell from 56 to 20 ms, and syscalls per move at human pace fell from 21 to 4.

## The orders of magnitude

A request's server cost fell by five to six orders of magnitude: from hundreds of milliseconds on Supabase to about a microsecond for the game work itself.
The top rows are wall time spent waiting on network services, and the lower rows are CPU time, because once the waits were gone CPU was all that was left.

| Stage | A move costs | What it is |
|---|---|---|
| Supabase edge functions + PostgREST (to Sep 2026) | about 450 ms warm, over 1.3 s cold | production log of an edge function in July 2026 (#41): JWT verify 88 ms warm / 565 ms cold, then the PostgREST call 370 ms warm / 753 ms cold, in sequence. A cold isolate, recycled about every 15 s, added a ~760 ms floor. The SQL itself ran in 0.07 ms, and a 4-seat `commit_table` took 3.5-3.8 ms against local PostgREST |
| Lambda + DynamoDB, before #372 | about 55 ms | 15-17 ms p50 for the DynamoDB TransactWriteItems, plus five IoT publishes in a row at 7.8 ms each |
| Lambda + DynamoDB, after #372 | about 10 ms | 2.4 ms for a single conditional UpdateItem, plus one push batch sent in parallel (about 8 ms) |
| VM at cutover (#391), SQLite group commit | under 1 ms | move p99 0.8-0.9 ms; 182 µs of server CPU per move on the nano |
| Today's main, the whole server | about 100 µs or less | move p99 0.40 ms; 67 µs/move on the rig and 84 µs on the nano (NOW build); most of it is kernel TCP, network interrupts and waking the CPU |
| Today's main, our own code at saturation | about 15-20 µs | the user-space share of sat2's 36 µs/move |
| The game work itself, warm | about 1 µs | rules, journal record and outputs; the rules' apply alone is 250-300 ns |

The Supabase row is from a list read's edge function, `get_my_games`, because that is the call that was timed in production; a move went through the same kind of edge function with the same JWT check and the same PostgREST hop, plus the `commit_table` transaction.
The Supabase sources are commits 4d9e5874, 10658d3b and 9dab342e0, and c_base/docs/DIRECT_DYNAMO.md.
The Lambda rows come from #365 and #372: #365 also cut Lambda's server CPU from about 440 to about 300 µs per move, but the move's time was the network waits.

## The VM path (built 2026-10-18 and 2026-10-19, shipped in #391)

**One process on a t4g.nano instead of Lambda and DynamoDB.**
A request now runs inside one long-lived `cbase serve` process with SQLite on local disk.
That removes the per-request cold start, the DynamoDB network round trips and SigV4 signing, and the IoT/API Gateway hop for realtime.

**Group commit.**
Concurrent writes are merged into one SQLite transaction, with a savepoint per request, and each caller returns after the shared COMMIT.
Many moves pay for one BEGIN, one COMMIT and one WAL sync instead of one each.

**Checkpoints off the commit path.**
A separate thread with its own connection runs SQLite's WAL checkpoints every 1000 frames.
A commit never pays for a checkpoint, and a writer waits only if the WAL reaches its 8192-frame cap.

**Per-thread commit batch and kernel section.**
The 1 MB commit batch used to be malloc'd and memset on every commit attempt, and the 217 KB kernel section calloc'd on every operation.
Both became per-thread storage that is reset by zeroing only the part a use touched.

**The right number of request workers.**
The worker count stopped being a flat 4 and followed the CPU quota.
At a 1-CPU quota, one worker moved 2.1x the games of four (12.4k vs 5.8k moves/s), because four workers mostly fought over the same core.

**A binary access token with a verified-token cache.**
The JWT's JSON header and claims became a fixed binary layout plus HMAC-SHA256.
A per-thread cache of verified tokens lets a repeat request skip the HMAC, both base64 decodes and the claims parse, while expiry is still checked on every call.

**An eventfd wake instead of a self-pipe.**
On Linux the loop's wake channel is an eventfd: one fd and a counter, with no pipe buffer to copy through.
`pipe_write` had been 3.1% of a saturated server.

**A pooled request is one block from a free list.**
Handing a request to the pool used to cost two mallocs and two frees.
It is now one block from a free list owned by the loop thread.

**No mmap or munmap per move.**
musl's allocator unmapped an 8 KB group whenever its last slot was freed, and the SQLite store's 1.5-1.8 KB row buffers hit that on every move.
The SQLite pool now covers those blocks, and mmap calls per move at human pace fell from 1.07 to 0.02.

**One send per connection per loop pass.**
`cb_ws_send` queues, and the loop flushes each marked connection once at the end of its pass, so a commit's view and cast to one player leave in one `send()`.
sendto calls per move fell from 4.2 to 2.7.

**Hybrid inline dispatch and pings on the loop.**
Game reads and moves run directly on the loop thread when the request pool is idle and nothing else is waiting in the pass, so they skip the hand-off to a worker.
A WebSocket ping is answered on the loop with no allocation and no pool hand-off.

## After the cutover (2026-10-19 onward)

**#395 The move journal.**
A move is acknowledged after it is appended to an mmap'd journal segment, and SQLite is written only at game end, every 10 minutes or at a size cap.
At saturation this took the rig from 54 to 42 µs/move, syscalls from 20.3 to 11.5, and WAL per move from 9.8 KB to 16 B of journal.
On the nano, human pace went from 182 to 106 µs/move and 21.9 to 10.3 syscalls, and saturation from 10.0k to 13.8k moves/s.

**#396 Move path quick wins.**
Allocations per move fell from 41 to 2.
The unread actor envelope and the RAND_bytes call came off the move path, and the journal records the raw action instead of building the legal menu.
Saturation went from 40.6 to 33.5 µs/move and syscalls from 11.0 to 7.3.

**#397 One owner of game state.**
The journal's overlay holds the decoded game, and the app's separate row cache, a second owner, was deleted.
A move no longer decodes and re-encodes the game from its row.
Saturation went from 33.7 to 31.6 µs/move, and mallocs per move from 2.0 to 1.4.

**#399 The move on the loop.**
Socket and HTTP moves run inline on the loop thread, the loop writes the journal itself, and nothing on the move path waits on another thread.
Human pace went from 105 to 71 µs/move, syscalls from 8.6 to 3.9 and p99 from 0.59 to 0.39 ms.
Saturation went from about 31 to 26 µs/move and syscalls from 6.8 to 4.9.

**#402 and #404: the game-end regression's two causes.**
The overload valve had read wall time, so a vCPU parked by the hypervisor looked like a slow handler and sent every move to the pool for seconds.
It now reads thread CPU, and the SQLite flush runs as bounded transactions.
sat2 went from 49.6 to 41.0 µs/move, from 19.8k to 24.1k moves/s, and p99 from 37.6 to 26.4 ms.

**#405 One log buffer per game and a 16 MiB cap.**
Each game keeps one growing log buffer, the finalize scratch is per thread, and the waiting-move total is capped at 16 MiB.
The valve reads the cheap vDSO clock and takes the thread-CPU clock only on a suspect run.
hum2 syscalls went from 5.98 to 3.96, sat2 RSS from 67 to 45 MB and mallocs from 2.67 to 1.74.

**#406 A rated end stays on the loop.**
The rating has one owner, in the resident roster, and is written as a journal record, so ending a rated game writes no SQLite before the answer.
The same PR fixed a p99 regression: the pass budget had charged throttle stalls as work.
sat2 p99 went from 31 to about 20 ms.

**#407 and #408 Delta casts.**
A broadcast carries the moves instead of whole boards, and the client's kernel rebuilds the boards byte for byte.
Cast bytes per move fell from 244 to 58 at 2 seats and from 1,190 to 248 at 8 seats plus a spectator, and production frames from about 280 to about 108 bytes.
Cast CPU fell by about 60%.

**#411 An idle box without ticks, and a cheaper loop pass.**
The flush thread sleeps until a real deadline, and the loop's timer lives in a timerfd that is re-armed only when its deadline changes.
Each listener's pushes in a pass share one walk of the user table, and no spectators' cast is built for a room nobody watches.
sat2's slowest move went from 241 to 82 ms, and pushes over 100 ms from 94 to 0.

**#412 A fixed journal record with CRC-32C.**
The journal record reuses the move list the delta cast already builds, so the 722 B per-move copy and its memset are gone.
The checksum moved from byte-wise FNV-1a to CRC-32C, which uses Graviton's CRC32 instructions.

**#413 One request worker.**
The server runs three threads by default: the loop, one request worker and one compute thread.
That means one fewer thread to wake per game end and per bot cycle, and no allocator contention from a second worker.
Per move it was flat, with a shorter sat2 tail (max 96 to 63 ms).

**#414 No view for a player who is already in the game's room.**
The player in the room gets the cast and animates from it, so the per-move view to that same connection was dropped.
The dashboard and other connections still get it.
This saves bytes per move, and pushes over 100 ms went from 6 to 0.

**#418 A move's outputs written once.**
On a plain move the kernel no longer builds the spectators' view, which no one reads, and it no longer re-validates a roster that hasn't changed.
The roster trailer is written once per commit and copied into each player's envelope, and the bytes on the wire are unchanged.
sat2 dropped 0.7 µs/move, with 2.2% more moves/s.

**#419 No seat lists and one clock read per plain move.**
A plain move into the journal zeroes only the commit's scalar head and skips the joined and left lists.
The journal's stamps come from the loop's pass clock, so a move makes one clock read instead of several.

**#420 One game lookup per move, and the resident swapped instead of copied.**
An operation holds its game, so the fence and the apply use the held game instead of looking it up again: one lookup per move instead of four.
The new resident state is handed over by swapping two buffers instead of a 2 KB copy.

**#421 Bots ask whether a seat can move, not for its whole menu.**
`legal_any` answers the bot drive's question directly instead of building every legal move.
The change also fixed a kernel bug in which a bot's recorded move carried leftover bytes.

## Decided by the Graviton pass (pending)

The CPU knob (#416, `CBASE_LOOP_CPU` / `CBASE_BG_CPU`) and io_uring (#400, `CBASE_IO_URING`) are both off by default.
The nano run will say whether either should ship, and what that run shows will be added here.
