# adventurer/server: wandrer.earth rebuilt as a second product on c_base (../DESIGN.md).
#
#   make              build/adventurer (`adventurer serve`), build/adv-import, build/adv-gen,
#                     build/adv-wiregen and the generated web decoder (../web/src/gen/wire.ts)
#   make test         every unit test (test/unit_*.c), each its own program
#   make check        test, then the smoke (a real server, a real upload, a real tile)
#   make check-asan   the same, every object rebuilt under ASan + UBSan
#   make fuzz         build/fuzz/<target> for every fuzz/fz_*.c (libFuzzer; Homebrew LLVM on macOS)
#   make fuzz-smoke   each fuzz target for FUZZ_TIME seconds (default 20)
#   make graph        data/region.graph from data/region.osm.pbf (make import first; the pbf is
#                     downloaded by ../tools/fetch_osm.sh and is NOT in git)
#   make clean
#
# THE PLATFORM IS c_base's, BY SOURCE: every .c under c_base/core/src, c_base/providers/local/src
# and c_base/providers/aws/src (product-free, docs/PLATFORM.md 8.4; the aws provider comes along
# because core's store opener names it) is compiled here with adventurer's flags, exactly as
# c_base/Makefile compiles them for foolish, and no file of c_base's is copied or edited. The
# kernel foolish links is not here: core names nothing of it. The product is every .c under src/.
#
# NO FILE LISTS TO EDIT: every unit_*.c under test/ is a test program, every lib_*.c there is
# linked into each, every fz_*.c under fuzz/ is a fuzz target.

C_ROOT := ../..
CB     := $(C_ROOT)/c_base
UNAME  := $(shell uname)
.DEFAULT_GOAL := all

ifeq ($(UNAME),Darwin)
  OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
  SQLITE_PREFIX ?= $(shell brew --prefix sqlite 2>/dev/null)
  ifeq ($(wildcard $(SQLITE_PREFIX)/include/sqlite3.h),)
    $(error no Homebrew sqlite (SQLITE_PREFIX='$(SQLITE_PREFIX)'): brew install sqlite)
  endif
endif
ifneq ($(strip $(OPENSSL_PREFIX)),)
  OPENSSL_CFLAGS := -I$(OPENSSL_PREFIX)/include
  OPENSSL_LDFLAGS := -L$(OPENSSL_PREFIX)/lib
endif
ifneq ($(strip $(SQLITE_PREFIX)),)
  SQLITE_CFLAGS := -I$(SQLITE_PREFIX)/include
  SQLITE_LDFLAGS := -L$(SQLITE_PREFIX)/lib
endif

PLATFORM_ROOTS := $(CB)/core/src $(CB)/providers/local/src $(CB)/providers/aws/src
PLATFORM_MAINS := $(CB)/providers/local/src/main_serve.c $(CB)/providers/aws/src/main_lambda.c
PLATFORM_SRC := $(sort $(filter-out $(PLATFORM_MAINS),$(foreach r,$(PLATFORM_ROOTS),$(wildcard $(r)/*.c $(r)/*/*.c))))
SHARED_SRC := $(C_ROOT)/shared/c/b64/b64.c $(C_ROOT)/shared/c/hex/hex.c $(C_ROOT)/shared/c/crc32c/crc32c.c $(C_ROOT)/shared/c/mqtt/mqtt.c $(C_ROOT)/shared/c/sha256.c
GEN_SRC := build/gen/schema_sql.c build/gen/console_html.c build/gen/adv_schema_sql.c
# The product: src/ and its one directory level, but for the mains under tools/ and gen/.
APP_SRC := $(sort $(wildcard src/*.c src/*/*.c))
LIB_SRC := $(PLATFORM_SRC) $(SHARED_SRC) $(GEN_SRC) $(APP_SRC)

PLATFORM_INC := -I$(CB)/core/include -I$(CB)/core/src -I$(CB)/providers/local/src -I$(CB)/providers/aws/src -I$(C_ROOT)/shared/c
INC := $(PLATFORM_INC) -Isrc -Itest -Ifuzz -I$(CB)/core/test
CFLAGS = -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wvla \
         $(INC) $(SQLITE_CFLAGS) $(OPENSSL_CFLAGS) -pthread
LDLIBS := $(SQLITE_LDFLAGS) $(OPENSSL_LDFLAGS) -lsqlite3 -lssl -lcrypto -lz -lpthread -lm

SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
ASAN_CFLAGS = $(filter-out -O2,$(CFLAGS)) -O1 $(SAN) -DCB_INSTRUMENTED -DADV_INSTRUMENTED

include $(C_ROOT)/shared/tools/c_rules.mk
$(eval $(call C_VARIANT,build/obj/rel,CC,CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,ASAN_CFLAGS))

# The platform's schema and console, and the product's schema, embedded as c_base embeds them.
build/embed: $(CB)/tools/embed.c
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@
build/gen/schema_sql.c: $(CB)/providers/local/sql/schema.sql build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_schema_sql > $@.new && mv $@.new $@
build/gen/console_html.c: $(CB)/core/src/admin/console.html build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_console_html > $@.new && mv $@.new $@
build/gen/adv_schema_sql.c: sql/schema.sql build/embed
	@mkdir -p $(@D)
	build/embed $< adv_schema_sql > $@.new && mv $@.new $@

TEST_SRC := $(sort $(wildcard test/*.c))
# The platform's test client and child-process helpers (c_base/core/test), linked into every test
# as they are into c_base's own: the smoke drives a real server through them.
TLIB_SRC := $(sort $(wildcard test/lib_*.c)) $(CB)/core/test/lib_client.c $(CB)/core/test/lib_proc.c
UNIT_SRC := $(sort $(wildcard test/unit_*.c))
UNITS    := $(sort $(basename $(notdir $(UNIT_SRC))))
SMOKES   := $(sort $(basename $(notdir $(wildcard test/smoke*.c))))

# $(call prog,<outdir>,<variant>,<name>,<sources>,<flags var>)
define PROG
$(1)/$(3): $$(call c_objs,$(2),$(4) $$(LIB_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(CC) $$($(5)) $$^ -o $$@ $$(LDLIBS)
endef
prog = $(eval $(call PROG,$(1),$(2),$(3),$(4),$(5)))

$(call prog,build,build/obj/rel,adventurer,tools/main_serve.c,CFLAGS)
$(call prog,build,build/obj/rel,adv-import,tools/import_main.c,CFLAGS)
$(call prog,build,build/obj/rel,adv-gen,$(sort $(wildcard gen/*.c)),CFLAGS)
$(call prog,build,build/obj/rel,adv-wiregen,tools/wiregen.c,CFLAGS)
$(call prog,build,build/obj/rel,adv-bench,tools/bench_match.c,CFLAGS)
$(call prog,build/asan,build/obj/asan,adventurer,tools/main_serve.c,ASAN_CFLAGS)
$(call prog,build/asan,build/obj/asan,adv-import,tools/import_main.c,ASAN_CFLAGS)
$(call prog,build/asan,build/obj/asan,adv-gen,$(sort $(wildcard gen/*.c)),ASAN_CFLAGS)
$(foreach u,$(UNITS) $(SMOKES),$(call prog,build/test,build/obj/rel,$(u),test/$(u).c $(TLIB_SRC),CFLAGS))
$(foreach u,$(UNITS) $(SMOKES),$(call prog,build/asan/test,build/obj/asan,$(u),test/$(u).c $(TLIB_SRC),ASAN_CFLAGS))

# The web's decoder of the binary surface (src/wire.h), written by the C compiler's own
# offsetof and sizeof, so no TypeScript ever spells a byte offset by hand.
WIRE_TS := ../web/src/gen/wire.ts
$(WIRE_TS): build/adv-wiregen
	@mkdir -p $(@D)
	build/adv-wiregen > $@.new && mv $@.new $@

.PHONY: all test smoke check test-asan smoke-asan check-asan clean graph import fuzz fuzz-smoke
all: build/adventurer build/adv-import build/adv-gen build/adv-wiregen build/adv-bench $(WIRE_TS)
import: build/adv-import

# Every test program is its own process; they run CHECK_JOBS at a time, each to its own log,
# every one runs even when another fails, and the target names each that did.
CHECK_JOBS ?= $(shell getconf _NPROCESSORS_ONLN)
RUN_ALL_SH = mkdir -p build/logs && printf '%s\0' $(2) | xargs -0 -P $(CHECK_JOBS) -n 1 sh -c \
  'log=build/logs/$$(echo "$$0" | tr " /=" "___").log; if eval "$$0" > $$log 2>&1; then cat $$log; else cat $$log; echo "FAILED: $$0"; exit 1; fi' \
  || { echo "$(1) FAILED"; exit 1; }
define RUN_ALL
	@$(call RUN_ALL_SH,$(1),$(2))
endef
UNIT_RUNS  = $(addprefix $(1)/test/,$(UNITS))
SMOKE_RUNS = $(foreach s,$(SMOKES),'$(1)/test/$(s) $(1)/adventurer $(1)/adv-gen')
SMOKE_PROGS = $(1)/adventurer $(1)/adv-gen $(addprefix $(1)/test/,$(SMOKES))

test: $(call UNIT_RUNS,build)
	$(call RUN_ALL,UNIT TESTS,$(call UNIT_RUNS,build))
smoke: $(call SMOKE_PROGS,build)
	$(call RUN_ALL,SMOKE,$(call SMOKE_RUNS,build))
check: $(call UNIT_RUNS,build) $(call SMOKE_PROGS,build)
	$(call RUN_ALL,CHECK,$(call SMOKE_RUNS,build) $(call UNIT_RUNS,build))
test-asan: $(call UNIT_RUNS,build/asan)
	$(call RUN_ALL,ASAN UNIT TESTS,$(call UNIT_RUNS,build/asan))
smoke-asan: $(call SMOKE_PROGS,build/asan)
	$(call RUN_ALL,ASAN SMOKE,$(call SMOKE_RUNS,build/asan))
check-asan: $(call UNIT_RUNS,build/asan) $(call SMOKE_PROGS,build/asan)
	$(call RUN_ALL,ASAN CHECK,$(call SMOKE_RUNS,build/asan) $(call UNIT_RUNS,build/asan))

# ---- the graph -------------------------------------------------------------------------------
DATA ?= ../data
PBF  ?= $(DATA)/region.osm.pbf
GRAPH ?= $(DATA)/region.graph
graph: $(GRAPH)
$(GRAPH): build/adv-import $(PBF)
	build/adv-import $(PBF) $@

# ---- fuzz (c_base/fuzz/fuzz.mk's shape, for adventurer's own targets) -------------------------
ifeq ($(UNAME),Darwin)
  FUZZ_CC ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang) clang)
else
  FUZZ_CC ?= clang
endif
FUZZ_TIME ?= 20
FUZZ_SAN := -fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined -fno-omit-frame-pointer
FUZZ_CFLAGS = $(filter-out -O2,$(CFLAGS)) -O1 $(FUZZ_SAN) -DCB_INSTRUMENTED -DADV_INSTRUMENTED
$(eval $(call C_VARIANT,build/obj/fuzz,FUZZ_CC,FUZZ_CFLAGS))
FUZZ_SRC := $(sort $(wildcard fuzz/fz_*.c))
FUZZ_TARGETS := $(sort $(patsubst fz_%.c,%,$(notdir $(FUZZ_SRC))))
define FUZZ_PROG
build/fuzz/$(1): $$(call c_objs,build/obj/fuzz,fuzz/fz_$(1).c $$(LIB_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(FUZZ_CC) $$(FUZZ_CFLAGS) -fsanitize=fuzzer $$^ -o $$@ $$(LDLIBS)
endef
$(foreach t,$(FUZZ_TARGETS),$(eval $(call FUZZ_PROG,$(t))))
fuzz: $(addprefix build/fuzz/,$(FUZZ_TARGETS))
fuzz-smoke: fuzz
	@set -e; for t in $(FUZZ_TARGETS); do \
	  mkdir -p build/fuzz-corpus/$$t build/fuzz-crashes/$$t fuzz/corpus/$$t; \
	  echo "fuzz $$t for $(FUZZ_TIME)s"; \
	  build/fuzz/$$t -max_total_time=$(FUZZ_TIME) -max_len=65536 -artifact_prefix=build/fuzz-crashes/$$t/ build/fuzz-corpus/$$t fuzz/corpus/$$t 2> build/fuzz-$$t.log || { tail -40 build/fuzz-$$t.log; exit 1; }; \
	  tail -1 build/fuzz-$$t.log; done

clean:
	rm -rf build
