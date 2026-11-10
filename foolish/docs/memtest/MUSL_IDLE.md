# musl static `cbase serve`: idle RSS breakdown (2026-11-09)

## Build and run

Source: foolish origin/main 400a65d097557132a2eb057b4f0833a9b7929ff9 ("web.yaml: /m/* goes to the VM, as the live distribution does"), exported with `git archive` to verify/src_main/.
Build: the pinned image from c_base/providers/aws/ops/build_bootstrap.sh (alpine:3.24@sha256:294b683c...), `apk upgrade`, the same apk set, `make STATIC=1 build/cbase`, then `strip` (verify/musl2/scripts/build.sh).
Toolchain after `apk upgrade`: gcc 15.2.0-r5, musl 1.2.6-r3, openssl 3.5.9-r0, sqlite 3.53.4-r0 (verify/musl2/out/versions.txt).
The binary is static-pie (Alpine's gcc defaults to PIE): one R E LOAD of 7,684 KiB, one RW LOAD whose file part is 516 KiB plus a 14,624 KiB .bss, PT_TLS memsz 0x1a4e90 (1,723,024 bytes, .tdata 228 bytes).
The unstripped build (cbase.g) has the same loaded bytes; a relink of the same objects is byte-identical to it, and its idle RSS matches the stripped one (7,048-7,052 KiB against 7,044-7,052).
Run: inside that Alpine container on the Mac (Docker Desktop, arm64, kernel 6.12.54-linuxkit, 4 KiB pages), binary at /usr/local/bin, cwd /var/lib/cbase.
Env: the cbase.service shape (CBASE_STORE=sqlite, CBASE_DB=/var/lib/cbase/cbase.sqlite, CBASE_LISTEN=:::8080, SSL_CERT_FILE) plus the vm_boot.sh keys with throwaway dummy values (CBASE_JWT_SECRET, CBASE_ADMIN_TOKEN, CBASE_ORIGIN_SECRET, CBASE_PUBLIC_URL, CBASE_ALLOWED_ORIGINS).
CBASE_BACKUP_BUCKET is unset, so the backup path (and whatever it allocates) is not exercised.
Fresh empty database each run, server on CPUs 0-1, no clients, captured 20-22 s after "listening on".
The docker kernel runs THP=always; every number below is taken with THP disabled for the process (prctl PR_SET_THP_DISABLE via verify/musl2/scripts/nothp.c), which is what a THP=madvise or never host gives, see Caveats.

## Headline

Idle VmRSS is 7,044-7,052 KiB (6.9 MiB) over five runs, not 4.7 MiB: RssAnon 1,952-1,960 KiB, RssFile 5,092 KiB.
The 5 MiB of file-backed pages are clean pages of the binary itself, so 72% of idle RSS is reclaimable text and rodata, and only about 2.0 MiB is private dirty.
The earlier 4,796 KiB (out/run/status_musl_idle.txt, build of 284f0dc4f) had RssFile 3,236 and RssAnon 1,560.
The file part differs because file-backed RSS is whatever fault-around maps from the page cache: with the binary hot in the cache (just copied, as on a VM that just downloaded it) the same code paths map 5 MiB; with a colder cache fewer pages are mapped.
The 400 KiB anon difference to the earlier pass is not attributed (284f0dc4f was not rebuilt here).

## Idle table (sorted by resident)

Run st3 (verify/musl2/out/smaps_st3.txt, cbase.g under strace -f -k, VmRSS 7,052 KiB, the same as untraced runs).
Composition of each anon mapping comes from replaying every mmap/munmap in strace_idle3.txt and testing each page's present bit in /proc/PID/pagemap in that same process (live_idle3.txt), so merged VMAs are split back into the calls that made them.
Image sections come from the pagemap present bits mapped onto `readelf -S` (pagemap_st3.txt).

```
mapping (size)                    KiB res  priv dirty  file-backed  what it is                                                  evidence
--------------------------------  -------  ----------  -----------  ----------------------------------------------------------  -----------------------------
binary R E LOAD (7,684K)            5,056           0        5,056  .text 3,732; .rela.dyn 656; .rodata 644; .eh_frame_hdr 32  pagemap_st3 SEC lines
                                                                    (152 of it Shared_Clean, the rest Private_Clean)
anon (2,436K, merged)                 620         620            0  sqlite prepared statements 504, sqlite page cache 100,     live_idle3 per-VMA line
                                                                    checkpointer thread map (stack+TLS) 16
binary RW LOAD, file part (516K)      516         516            0  .data.rel.ro 472 dirtied by static-pie self-relocation     pagemap_st3; 27,873 R_AARCH64_RELATIVE
                                                                    (27,873 RELATIVE relocs), .got, .data 44                   in readelf -r; no-pie test below
anon (372K)                           328         328            0  OpenSSL init tables 208, checkpointer page cache 84,       live_idle3; wrap_idle_top.txt
                                                                    cb_auth_new 28, cb_oidc_new 8, cb_server_new 4
anon hole between the LOADs (100K)    100         100            0  mmaps the kernel placed in the 100K hole between the R E  live_idle3 (all 100K are strace-seen
                                                                    and RW LOADs: checkpointer lookaside 68, page cache 28      mmaps, not ELF)
anon (88K)                             88          88            0  sqlite page cache, writer connection (pcache1)             live_idle3
anon (84K)                             84          84            0  sqlite lookaside, writer connection 68; statements 16      live_idle3
anon (4,064K, merged)                  48          48            0  log ring calloc 573,456 B (20 resident), one pool thread   strace: cb_log_init, cb_pool_new,
                                                                    map (16), main thread static TLS mmap 1,724,296 B (12)     static_init_tls
.bss (14,624K)                         40          40            0  .bss pages touched at idle                                  pagemap_st3
anon (36K)                             36          36            0  sqlite statements 28, store 4, loop 4                       live_idle3
anon (52K)                             32          32            0  sqlite statements, cb_journal_open buffer (18,728 B)       live_idle3; wrap journal.c:2657
cbase.sqlite-shm (32K, MAP_SHARED)     32           0           32  WAL index, one 32 KiB shm region                            smaps (rw-s)
[stack] main thread (132K)             28          28            0  main thread stack                                           smaps
[heap] brk (20K)                       20          20            0  mallocng meta area (alloc_meta grows brk)                   strace brk calls under cb_log_init
anon (1,816K) journal thread map       12          12            0  journal thread: 128K stack + 1,723,024 B TLS, 12 touched   strace: __pthread_create<cb_journal_open
anon (1,816K) pool thread map           8           8            0  the other pool worker thread map, 8 touched                strace: __pthread_create<cb_pool_new
[vdso]                                  4           0            4  vdso                                                        smaps
--------------------------------  -------  ----------  -----------
total                               7,052       1,960        5,092
```

By owner, idle, private dirty only (1,960 KiB):
sqlite 1,012 KiB (two connections, the writer and the checkpointer thread's ck_main, each with its own 64 KiB lookaside and page cache, plus every statement prepared at open).
Static-pie relocation 472 KiB (.data.rel.ro) plus .got/.data 44.
OpenSSL init 208 KiB (wrapper: 139,732 live bytes through CRYPTO_malloc plus a 32 KiB CRYPTO_aligned_alloc hash table).
Five threads' stacks and TLS together 56 KiB resident out of 9.0 MiB virtual (four 1,867,776-byte thread maps plus the main thread's 1,724,296-byte TLS mmap).
cb_* allocations about 100 KiB (auth, oidc, server, journal buffer, log ring's touched 20 KiB, loop).
Heap brk 20, main stack 28, .bss 40.

## Allocation sites (counting malloc wrapper)

verify/musl2/scripts/wrap.c is linked into a second build with -Wl,--wrap=malloc,calloc,realloc,free,aligned_alloc,memalign,posix_memalign,malloc_usable_size,CRYPTO_malloc,CRYPTO_zalloc,CRYPTO_realloc.
It keys plain allocations by return address (symbolized with addr2line) and OpenSSL ones by the file:line OpenSSL passes in; SIGUSR2 dumps live bytes per site.
Its 16-byte header raises RSS (7,976 KiB idle), so it is used only for live byte counts, never for RSS.
Idle live heap: 1,662,684 bytes in 5,720 blocks (verify/musl2/out/wrap_idle_top.txt):
pm_malloc (store_sqlite.c:762, SQLite's allocator hook) 872,904 bytes in 2,361 blocks.
cb_log_init (log.c:27) 573,440 bytes in one calloc, of which only 20 KiB is ever touched at idle.
OpenSSL 139,732 bytes (lhash.c:130 42,288, hashtable.c:632 30,784, sparse_array.c:176 10,240, ...) plus CRYPTO_aligned_alloc 32,768.
cb_journal_open 18,728; cb_auth_new 8,792; cb_oidc_new 5,552; cb_server_new 2,432.

## Proven savings levers (measured, not implemented)

Static non-PIE: relinking the same objects with `-static -no-pie` gives idle VmRSS 6,536 KiB twice (RssAnon 1,472, RssFile 5,064), 516 KiB less than static-pie, nearly all of it private dirty (the 472 KiB .data.rel.ro no longer written at startup) (status_nopie1/2.txt).
Alpine's gcc builds PIE by default, so `-static` produces static-pie; ASLR of the image is what that would trade away.
The checkpointer's second SQLite connection (ck_main, store_sqlite.c:459) costs about 200 KiB of private dirty at idle (live_idle.txt, thread 3111: lookaside 68, page cache 84, unixOpen 16, Fts3 module 16, btree 8).

## Under sat2 (8 pairs, think 0-1 ms, 25 s)

Load: verify/musl2/scripts/sat2.sh, the pass's p3load3 client built statically from the same tree, the same server env plus the load limits the earlier pass used (CBASE_LIMIT_*=1000000, CBASE_RATED_MIN_AGE=0, CBASE_MAX_CONNS, CBASE_LISTEN_BACKLOG), and with CBASE_ORIGIN_SECRET and CBASE_ALLOWED_ORIGINS unset because the client sends no origin header (with them set every intent answers 403).
Result: 611,485 moves at 24.5k moves/s, p50 0.11 ms, p99 0.89 ms; RSS p50 41.6, p99 53.5, max 53.8, VmHWM 53.9 MiB (load_sat2a.txt, rss_sat2a.log).
Mid-run (22 s after go): VmRSS 51,556 KiB, RssAnon 41,648, RssFile 9,908 (smaps_sat2a_mid.txt).
What grows, from the wrapper build under the same load (44,974,431 live bytes, wrap_sat2_top.txt):
the journal's in-memory overlay is about 32.5 MB: res_copy journal.c:566 13.3 MB, row_refresh journal.c:534 10.2 MB, grow_copy journal.c:507 5.0 MB, ov_insert journal.c:620 3.8 MB, ov_apply 0.13 MB, each about 4,090 blocks, one per row touched.
SQLite pm_malloc 4.7 MB (2,823 blocks); finalize_game table_ops.c:488 3.4 MB in one block; coder_scratch_ready replay.c 1.9 MB in two; cb_section_new table_ops.c:271 0.96 MB; vcache_for jwt.c:104 0.26 MB.
File-backed growth: the journal segment cbase.sqlite-journal.<ts> is an 8,320 KiB MAP_SHARED file mapping with 4,628 KiB resident dirty, and text grows from 5,056 to 5,184 KiB.
.bss residency grows from 40 to 100 KiB.

Thread TLS touched under sat2 (pagemap present bits over [td, td+0x1a4e90) for each thread, td from gdb's pthread_self(); tls_sat2a.txt):
```
thread              TLS block   resident
main (loop)         1,684 KiB   20 KiB
req-0               1,688 KiB   40 KiB
job-0               1,688 KiB   36 KiB
journal / ckpt      1,688 KiB    8 KiB each
```
So under musl a working thread touches 36-40 KiB of its 1,688 KiB static TLS, about 2%, and the idle threads 8 KiB.
This answers VERIFY.md section 5's open question for sat2's work only: sat2 is two human seats per table, so no bot search ran (the log shows CBASE_BOT_WORKERS=1 but no bot activity), and a bot thread running the solver could touch more (solve_ws 278,592 B, five 136,080 B worlds in .tbss); that case is still unproven.

## Caveats

THP: the docker kernel is THP=always, and one idle run of five without the prctl came up at 10,816 KiB (RssAnon 5,724) because the 2.4-4 MiB anon VMAs got 2 MiB huge pages; on a host with THP=always the idle figure can jump by 2-4 MiB at random, so check the VM's /sys/kernel/mm/transparent_hugepage/enabled (not checked here).
File-backed RSS depends on fault-around and the page cache state, so the 5,056 KiB of text is an upper figure for a hot cache and is reclaimable; section attribution of it says which pages are mapped, not which were executed.
Run in Docker Desktop's linuxkit 6.12 kernel, not the VM's AL2023 kernel, and not under systemd (no ProtectSystem, PrivateTmp etc., which do not change the process's own mappings).
Mallocng groups are attributed to the call that made each mmap; later allocations of the same size class share a group, so per-subsystem splits inside a group are approximate (the totals per VMA are exact).
The backup path (CBASE_BACKUP_BUCKET) is not exercised, and no HTTPS outbound call (OIDC fetch, backup) happened, so OpenSSL's TLS client state is absent from these numbers.
The sat2 run needed the load limits and an open origin, so it is not the production env.
Raw data: verify/musl2/out/ (smaps, status, rollup, pagemap, strace, live, wrap dumps, gdb, load); scripts: verify/musl2/scripts/.
