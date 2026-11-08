# foolish/server - the foolish backend: the product on cbase (the cbase/ submodule, its cbase.mk),
# with the kernel (foolish/c) linked into every program.
#
#   make            build/cbase (`cbase serve`), build/bootstrap (the Lambda runtime), build/cbase-import,
#                   build/cbase-export, build/cbase-bench
#   make check      cbase's platform tests and smokes with this product linked, and the product's
#   make check-asan, check-dynamo, coverage, fuzz, fuzz-replay, ...   as cbase/cbase.mk defines them
CBASE  := ../../cbase
C_ROOT := ../..
CBASE_PROVIDERS := local aws
UNAME  := $(shell uname)
KDIR   := ../c

# ---- the kernel ------------------------------------------------------------------
# THE KERNEL'S OWN SOURCE LIST: foolish/c/Makefile's KERNEL_SRC, read through its print-kernel target,
# so a source added to the kernel is linked here without an edit. Its paths are relative to foolish/c,
# respelled through $(C_ROOT) so c_rules.mk places the objects under _root/. What cbase's lib/ links
# into every program already (sha256.c, mqtt/mqtt.c) is left out: one copy of each in a program.
KERNEL_LIST := $(shell MAKEFLAGS= $(MAKE) -s --no-print-directory -C $(KDIR) print-kernel)
ifeq ($(strip $(KERNEL_LIST)),)
  $(error could not read the kernel's source list (make -C $(KDIR) print-kernel))
endif
KERNEL_SRC := $(filter-out %/shared/c/sha256.c %/shared/c/mqtt/mqtt.c,\
              $(patsubst ../../%,$(C_ROOT)/%,$(filter ../../%,$(KERNEL_LIST))) \
              $(addprefix $(C_ROOT)/foolish/c/,$(filter-out ../../%,$(KERNEL_LIST))))

# THE KERNEL'S CAPACITIES for human games (foolish/c print-human-caps). They change struct sizes
# (Game, the snapshot stores), so every object is compiled with them (APP_CFLAGS reaches cbase's
# own objects too), or the two halves would disagree on a layout.
KERNEL_CAPS := $(shell MAKEFLAGS= $(MAKE) -s --no-print-directory -C $(KDIR) print-human-caps)
ifeq ($(filter -DMAX_LOG_PAIRS=%,$(KERNEL_CAPS)),)
  $(error could not read the kernel's human-game capacities (make -C $(KDIR) print-human-caps))
endif

# THE KERNEL'S OWN FLAGS (foolish/c/Makefile CFLAGS), with two differences, both on purpose:
#   * no -D_Thread_local= : cbase runs kernel calls on worker threads, and the kernel's per-thread
#     scratch must stay per thread.
#   * no -flto by default (`make LTO=1` turns it on for the kernel).
# -DCD_PASS_PROBE is a test counter of the kernel's own suite and is left out.
KERNEL_CFLAGS := -O3 -ffp-contract=off -fno-math-errno -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
                 -I$(KDIR)/src -I$(C_ROOT)/shared/c -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK $(KERNEL_CAPS)
ifneq ($(CPU),)
  KERNEL_CFLAGS += -mcpu=$(CPU)
endif
ifeq ($(LTO),1)
  KERNEL_CFLAGS += -flto
endif

APP_EXTRA_SRC         := $(KERNEL_SRC)
APP_EXTRA_CFLAGS      := $(KERNEL_CFLAGS)
# The kernel's own sanitized build turns object-size off (foolish/c/Makefile): replay.c's DealSlot
# is a Game cut deliberately short, and a finished game's replay code walks through it.
APP_EXTRA_ASAN_CFLAGS := $(filter-out -O3 -flto,$(KERNEL_CFLAGS)) -O1 -g -fsanitize=address,undefined \
                         -fno-sanitize-recover=undefined -fno-omit-frame-pointer -fno-sanitize=object-size
APP_EXTRA_FUZZ_CFLAGS := $(filter-out -O3 -flto,$(KERNEL_CFLAGS)) -O1 -g \
                         -fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined \
                         -fno-omit-frame-pointer -fno-sanitize=object-size
# Coverage: of the kernel only the files the binary surface is made of (netwire.c, netclient.c);
# the rest keeps its own flags, so the bots play at speed.
COVK_foolish/c/src/netwire   := -O0 --coverage
COVK_foolish/c/src/netclient := -O0 --coverage
APP_EXTRA_COV_CFLAGS   = $(KERNEL_CFLAGS) $(COVK_$*)
APP_COV_STAMP         := netwire+netclient:-O0 --coverage
APP_CFLAGS            := $(KERNEL_CAPS)
ifeq ($(UNAME),Darwin)
  APP_LDLIBS          := -framework Accelerate
endif

# ---- the product ---------------------------------------------------------------------------------
APP_SRC_ROOTS   := src
APP_TEST_ROOTS  := test
APP_INC         := -Isrc -I$(KDIR)/src
# The journal's checksum: shared/c's, compiled as the product's own.
APP_LIB_SRC     := $(C_ROOT)/shared/c/crc32c/crc32c.c
APP_EMBED       := sql/schema.sql=cb_foolish_schema_sql
APP_PROGS       := cbase-import=migrate/import.c cbase-export=migrate/export.c
# The /m/<payload> cases test/fixture_unfurl.h holds, made by the kernel: run by hand when the
# goldens are regenerated, never by check.
APP_TOOLS       := unfurl-payloads=tools/unfurl_payloads.c
APP_SMOKES      := smoke smoke_matrix smoke_authz smoke_lambda_after
SMOKE_ARGS_smoke_lambda_after = $(1)/$(APP_LAMBDA)
APP_LAMBDA_SCHEDULE := scan
APP_LAMBDA_JOB  := bot_drive
APP_GATE_HEADER := store/rows_foolish.h
APP_NAME_ALLOW  := test/name_allow.txt
APP_JSON_ROOTS  := src api bench migrate sql tools
APP_JSON_FILES  := src/app/admin_json.c
APP_FUZZ_ROOTS  := fuzz
APP_FUZZ_SEEDS  := fuzz/seeds_app.c
# The sequence target makes the process's randomness a function of the input.
FUZZ_EXCLUDE_sequence := $(CBASE)/core/src/util/random.c
APP_BENCH_ROOTS := bench
APP_URING_COV_UNITS := unit_inline_moves
COV_ROOT        := ../..
CB_COV_STRIP    := cbase/
APP_COV_FILTER  := --filter 'foolish/server/src/' --filter 'foolish/c/src/net(wire|client)\.c'
APP_COV_FLOOR   := test/cov_floor.txt
# check-dynamo: the store, admin, import and binary-surface tests and the game smokes against
# DynamoDB Local, the schema's bots seeded as a deploy seeds them, and the key-capture golden.
APP_DYNAMO_DEPS   := $(addprefix build/test/,unit_store unit_admin unit_import unit_v1 unit_export smoke smoke_matrix) \
                     build/cbase build/cbase-import build/cbase-export
APP_DYNAMO_SEED   := build/cbase-import --seed-bots
APP_DYNAMO_RUNS   := 'build/test/smoke build/cbase' 'build/test/smoke_matrix build/cbase' \
                     build/test/unit_store build/test/unit_admin build/test/unit_import build/test/unit_v1 \
                     'env -u CBASE_DYNAMO_CAPTURE build/test/unit_export build/cbase-export'
APP_DYNAMO_GOLDEN := test/golden/dynamo_shapes.txt
# What the static build (cbase/ops/build_static.sh: make bootstrap, make serve-binary) copies into
# its container: the trees this build reads.
CBASE_BUILD_COPY := cbase foolish/c foolish/server shared
include $(CBASE)/cbase.mk

# shared/c's tests of what the product links from there (the journal's CRC-32C).
SHARED_TESTS := crc32c_test
include $(C_ROOT)/shared/tools/shared_tests.mk
# Only its own rule: the tests cbase's lib/ has (b64_test, hex_test, ...) are cbase's (lib-tests).
$(eval $(call SHARED_TEST_RULE,,build/obj/cbase,CC,CB_CFLAGS,crc32c_test))
$(eval $(call SHARED_TEST_RULE,asan_,build/obj/asan,CC,ASAN_CB_CFLAGS,crc32c_test))
test check: shared-tests
check-asan test-asan: shared-tests-asan

print-kernel-src:
	@echo $(KERNEL_SRC)
