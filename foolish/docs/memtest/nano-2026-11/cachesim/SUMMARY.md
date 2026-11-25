# Cache model of the cbase server under sat2 (valgrind, not hardware)

This is a model of the instruction stream, run in valgrind's cache simulator, and it is not a hardware measurement.
The nano has no L1 or L2 counters, so these numbers are the only L1 and L2 view of the move path we have.
The binary is the dynamic glibc build of commit 9c5057b64 (the same snapshot as the Graviton build) with -O2 -g and frame pointers, in an Amazon Linux 2023 aarch64 container with valgrind 3.19.0.
The server ran in a container with 2 CPUs and 412 MiB, the same limits as the nano.
Load was the harness's load.sh driving p3load, 2 pairs, think 0-1 ms, 25 s of play.

## Runs

The Neoverse N1 model is --I1=65536,4,64 --D1=65536,4,64 --LL=1048576,8,64, where the 1 MiB LL stands for the private L2.
The second model is --I1=65536,4,64 --D1=131072,8,128 --LL=12582912,12,128.
Each geometry has a play run and a setup run (boot, sign-ups, and a 1 s play window), and per-move values are (play minus setup) over the moves the load client counted in play minus setup.
The idle run (boot only, no client) is kept for reference.
Under valgrind the server moves about 1,000 to 2,400 moves per second, so runs are short and the per-move values are the product.

## Per-move totals (N1, play only, 57,994 moves)

Instructions (Ir) per move are 47,687.
Data reads 11,461 per move and data writes 8,038 per move.
L1 data misses are 81.6 read and 60.4 write per move; LL data misses are 3.2 read and 3.8 write per move.
Instruction L1 misses (I1mr) are 370.2 per move, and LL instruction misses are 2.6 per move.
Conditional and indirect branches are 7,171 per move, with 588.5 mispredicts per move (8.2 percent).
D1 miss rate is 0.73 percent of data accesses, LL miss rate 0.036 percent, and I1 miss rate 0.78 percent.

## Per-move totals (M1 geometry, play only, 39,213 moves)

Instructions per move are 47,633, almost the same as N1.
L1 data misses fall to 26.9 read and 13.2 write per move (N1: 81.6 and 60.4), a 3.5 times reduction in data misses.
I1 misses are unchanged at 365.6 per move, so instruction footprint is the L1 miss source that a 128 KiB D1 does not fix.
Mispredicts are 579.3 per move.

## Where the misses are (N1, per move, top of the list)

Top functions by data L1 read misses (D1mr): flush_txn in store/journal.c 14.7 per move; memcpy 8.2; flusher in journal.c 4.0; pub_deliver in rt_local.c 2.6; trim in journal.c 2.2; strcmp 1.7; router match 1.7; strcspn 1.2; cb_ws_send 0.9.
Top by LL data read misses (DLmr, per move): unattributed code 1.26; flush_txn in journal.c 0.91; memcpy 0.24; _int_malloc 0.14; _int_free 0.06; sqlite3_clear_bindings 0.06.
Top by D1 write misses (D1mw, per move): memset 23.5; memcpy 18.6; unattributed code 2.35; table_commit_products_flags 0.70; queue_frame 0.67; cb_loop_run 0.63.
Top by instructions (Ir): an unattributed bucket (23 percent of play Ir, see below), sorted_known.constprop.0 (6.7 percent), memcpy (6.1 percent), memset (3.3 percent), run_replay_v6 (3.2 percent), strlen (2.7 percent), pub_deliver (2.1 percent), state_put_rules (1.9 percent).
The full tables are in top25_Ir.txt, top25_D1mr.txt, top25_DLmr.txt and top25_D1mw.txt.

## Instruction classes of the hot functions

The histograms weight each instruction's play-only Ir from a callgrind run with --dump-instr=yes, and they are in iclass_top5.json.
sorted_known.constprop.0 is 57 percent other integer ALU, 23 percent branches, 12 percent loads, 5 percent multiplies and 3 percent stores.
run_replay_v6.part.0.constprop.0 is 48 percent other integer ALU, 18 percent branches, 17 percent loads, 8 percent stores, 6 percent multiplies, 2 percent divides, and it has a few SIMD and ldp/stp instructions.
pub_deliver is 59 percent branches, 19 percent other ALU, 14 percent loads and 3 percent stp; it has essentially no atomics.
state_put_rules is 47 percent other ALU, 21 percent branches, 15 percent loads and 10 percent stores.
replay_extras_encode_parts is 49 percent other ALU and 36 percent branches.
Across the part of the stream that maps into the server binary (58 percent of play Ir), there are about 63 integer divides and 8 atomics per move; the other integer class lumps compares, conditional selects and address arithmetic, so it is not a single instruction type.

## Unattributed code

About 23 percent of play Ir (632M of 2.77G) and 26 percent of play D1 read misses sit at addresses that valgrind could not name.
Their offsets (for example 0x2d2a0 to 0x313c4 and 0x50640) fall in code that is not in the server binary, most likely stripped internals of a shared library (libc, sqlite or libssl are candidates, not pinned down).
Libc's memcpy, memset, strlen and strcmp are named, but their per-instruction addresses are not mapped to file offsets, so their instruction classes are not in the histogram.

## Time model (estimate)

The model converts the N1 per-move counts to time with the Graviton numbers in FABLE_HANDOFF.md: L2 hit 6.0 ns, L3 hit 33 ns, mispredict 3 ns, divide 8.4 ns, atomic 7.6 ns, and 0.17 to 0.40 ns per instruction.
Base instructions give 8.1 to 19.1 us per move.
L1 misses that hit the 1 MiB LL (502.6 per move at 6.0 ns) add 3.0 us.
LL misses (9.6 per move at 33 ns) add 0.3 us, or 0.7 us more if they went to RAM.
Mispredicts (588.5 per move at 3 ns) add 1.8 us, divides add 0.5 us, and atomics add 0.06 us.
The modelled total is 13.8 to 24.8 us per move.
The measured nano figure is 41.5 us per move (24,113 moves per second with one loop thread busy).
The model covers about a third to a half of the measured time, and the rest is outside what valgrind counts: kernel time for sockets and epoll, and any per-instruction cost above the 0.4 ns upper bound.
The earlier perf run on the nano gave about 81k cycles per move over a 32 s window (about 32 us at 2.5 GHz) as an upper bound, which is consistent with kernel time being a large part of the gap.

## Limits

Valgrind does not count kernel instructions, so syscall and network time is absent from every number here.
Under valgrind, serialized threads change the move rate (60,077 moves in the first N1 play run, 40,553 in the M1 run, 26,176 in the callgrind run), so only per-move values compare across runs.
The setup subtraction removes boot and sign-ups, but it also removes the setup run's own 1 s of play, which is small.
Cachegrind (valgrind 3.19) does not take --dump-instr, so per-instruction data comes from callgrind, and its move count and totals are a separate run.
The LL in the model is private and 1 MiB, so LL misses are a proxy for going to the shared L3, and the 32 MiB L3 of the real chip is not simulated.
