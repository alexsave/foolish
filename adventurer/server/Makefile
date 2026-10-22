# adventurer/server: wandrer.earth rebuilt as a second product on c_base (../DESIGN.md).
#
#   make              build/adventurer (`adventurer serve`), build/adv-import, build/adv-gen,
#                     and the web's generated decoder (../web/src/gen/wire.ts, layout_hash.ts)
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
# Every output goes under $(B): `make B=build-exp DATA=../data-exp graph` builds and imports beside
# a running server without touching what it runs from (the live site is watched while work goes on).
B      ?= build
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
SHARED_SRC := $(C_ROOT)/shared/c/b64/b64.c $(C_ROOT)/shared/c/hex/hex.c $(C_ROOT)/shared/c/crc32c/crc32c.c $(C_ROOT)/shared/c/mqtt/mqtt.c $(C_ROOT)/shared/c/sha256.c \
              $(C_ROOT)/shared/c/stats/hist.c $(C_ROOT)/shared/c/proc.c
GEN_SRC := $(B)/gen/schema_sql.c $(B)/gen/console_html.c $(B)/gen/adv_schema_sql.c
# The product: src/ and its one directory level, but for the mains under tools/ and gen/.
APP_SRC := $(sort $(wildcard src/*.c src/*/*.c))
LIB_SRC := $(PLATFORM_SRC) $(SHARED_SRC) $(GEN_SRC) $(APP_SRC)

PLATFORM_INC := -I$(CB)/core/include -I$(CB)/core/src -I$(CB)/providers/local/src -I$(CB)/providers/aws/src -I$(C_ROOT)/shared/c
INC := $(PLATFORM_INC) -Isrc -Itest -Ifuzz -I$(CB)/core/test
CFLAGS = -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wvla \
         $(INC) $(SQLITE_CFLAGS) $(OPENSSL_CFLAGS) -pthread $(OBJ_FLAGS_$*)
# THE WIRE'S LAYOUT HASH (structgen, below) is compiled into wire.c alone, read from the file the
# generator wrote, so the server's copy is the generator's and never retyped; the file is in the
# variant's flag stamp, so a new hash rebuilds the objects (shared/tools/c_rules.mk).
OBJ_FLAGS_src/wire = -DADV_WIRE_HASH=$$(cat $(B)/layout/wire.hash)
WIRE_HASH_FILE := $(B)/layout/wire.hash
LDLIBS := $(SQLITE_LDFLAGS) $(OPENSSL_LDFLAGS) -lsqlite3 -lssl -lcrypto -lz -lpthread -lm

SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
ASAN_CFLAGS = $(filter-out -O2,$(CFLAGS)) -O1 $(SAN) -DCB_INSTRUMENTED -DADV_INSTRUMENTED

include $(C_ROOT)/shared/tools/c_rules.mk
$(eval $(call C_VARIANT,$(B)/obj/rel,CC,CFLAGS,$(WIRE_HASH_FILE)))
$(eval $(call C_VARIANT,$(B)/obj/asan,CC,ASAN_CFLAGS,$(WIRE_HASH_FILE)))

# The platform's schema and console, and the product's schema, embedded as c_base embeds them.
$(B)/embed: $(CB)/tools/embed.c
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@
$(B)/gen/schema_sql.c: $(CB)/providers/local/sql/schema.sql $(B)/embed
	@mkdir -p $(@D)
	$(B)/embed $< cbase_schema_sql > $@.new && mv $@.new $@
$(B)/gen/console_html.c: $(CB)/core/src/admin/console.html $(B)/embed
	@mkdir -p $(@D)
	$(B)/embed $< cbase_console_html > $@.new && mv $@.new $@
$(B)/gen/adv_schema_sql.c: sql/schema.sql $(B)/embed
	@mkdir -p $(@D)
	$(B)/embed $< adv_schema_sql > $@.new && mv $@.new $@

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

$(call prog,$(B),$(B)/obj/rel,adventurer,tools/main_serve.c,CFLAGS)
$(call prog,$(B),$(B)/obj/rel,adv-import,tools/import_main.c,CFLAGS)
$(call prog,$(B),$(B)/obj/rel,adv-gen,$(sort $(wildcard gen/*.c)),CFLAGS)
$(call prog,$(B),$(B)/obj/rel,adv-bench,tools/bench_match.c,CFLAGS)
$(call prog,$(B)/asan,$(B)/obj/asan,adventurer,tools/main_serve.c,ASAN_CFLAGS)
$(call prog,$(B)/asan,$(B)/obj/asan,adv-import,tools/import_main.c,ASAN_CFLAGS)
$(call prog,$(B)/asan,$(B)/obj/asan,adv-gen,$(sort $(wildcard gen/*.c)),ASAN_CFLAGS)
$(foreach u,$(UNITS) $(SMOKES),$(call prog,$(B)/test,$(B)/obj/rel,$(u),test/$(u).c $(TLIB_SRC),CFLAGS))
$(foreach u,$(UNITS) $(SMOKES),$(call prog,$(B)/asan/test,$(B)/obj/asan,$(u),test/$(u).c $(TLIB_SRC),ASAN_CFLAGS))

# THE WEB'S DECODER of the binary surface (src/wire.h) is structgen's (shared/tools/structgen,
# libclang): it reads the structs under this build's flags for this host and writes the readers
# (../web/src/gen/wire.ts) and the layout hash (layout_hash.ts), which is also what wire.c is
# compiled with (above). No TypeScript spells a byte offset, and a page built from another layout
# refuses the bytes. The hash file is remade on every make that needs it and rewritten only when
# it changes, as foolish's c/Makefile does; the modules are copied only when their content moved.
include $(C_ROOT)/shared/tools/gen_tools.mk
WIRE_TS ?= ../web/src/gen/wire.ts
WIRE_HASH_TS := $(dir $(WIRE_TS))layout_hash.ts
# The host's own layout (the server is native and the browser reads its bytes), so the host's
# triple and, on macOS, the SDK's headers, which libclang does not find by itself (as -isystem:
# the probe keeps the include and define flags of a build line and drops the rest, sg_clang.c).
SG_HOST_TRIPLE := $(shell $(CC) -dumpmachine)
ifeq ($(UNAME),Darwin)
  SG_SYSROOT := -isystem $(shell xcrun --show-sdk-path)/usr/include
endif
SG_WIRE_ROOTS := adv_w_list adv_w_user_def adv_w_user adv_w_region adv_w_user_stats adv_w_region_progress adv_w_leader \
                 adv_w_leaders adv_w_activity adv_w_accepted adv_w_ops adv_w_top_road adv_w_everyone
SG_WIRE_ARGS := --header src/wire.h --header src/trace.h --header src/match.h \
                $(foreach r,$(SG_WIRE_ROOTS),--root $(r) --snapshot $(r)) \
                --count adv_w_user_def.name=name_len --count adv_w_user.name=name_len --count adv_w_region.name=name_len \
                --count adv_w_top_road.name=name_len \
                --const ADV_W_KIND_ --const ADV_W_V --const ADV_W_NAME --const ADV_AF_ --const ADV_UK_ --const ADV_ACH_N --const ADV_TOP_N \
                --const ADV_T_ --const ADV_HC_ --const ADV_NO_REGION --const ADV_ACT_ --const ADV_MATCH_RJ_ --const ADV_NAME_MAX
$(WIRE_HASH_FILE): $(STRUCTGEN) FORCE
	@mkdir -p $(@D)
	@$(STRUCTGEN) --cwd . $(SG_WIRE_ARGS) --build 'adv=$(strip $(INC) $(SG_SYSROOT))' --target $(SG_HOST_TRIPLE) --print-hash \
	  --ts $@.ts --hash-ts $@.hash.ts > $@.new && \
	  { cmp -s $@.new $@ || { echo "wire layout: $$(cat $@ 2>/dev/null || echo none) -> $$(cat $@.new)"; mv $@.new $@; }; } && rm -f $@.new && \
	  $(call gen_sync,$@.ts,$(WIRE_TS)) && $(call gen_sync,$@.hash.ts,$(WIRE_HASH_TS))

# THE PLATFORM'S STREAM FRAMES (shared/c/stream_frame.h), for the page's feed socket: the heads
# of HELLO and SUB written, of REPLY read, by code structgen generates from the packed structs the
# hub's own parser is held to (stream_frame_test.c), so the page spells no byte offset of them
# either and needs no wasm. No layout hash: the frames carry their own version byte (SF_V).
SF_TS ?= ../web/src/gen/stream_frame.ts
SG_SF_ROOTS := sf_bare_hdr sf_hello_hdr sf_sub_hdr sf_reply_hdr
SG_SF_ARGS := --header $(C_ROOT)/shared/c/stream_frame.h $(foreach r,$(SG_SF_ROOTS),--root $(r) --snapshot $(r)) \
              --writer sf_hello_hdr --writer sf_sub_hdr \
              --const SF_V --const SF_HELLO --const SF_READY --const SF_SUB --const SF_UNSUB --const SF_REQ --const SF_REPLY \
              --const SF_PING --const SF_PONG --const SF_WHAT_ROOM --const SF_TOKEN_MAX --const SF_GID_MAX
$(B)/layout/stream_frame.stamp: $(STRUCTGEN) $(C_ROOT)/shared/c/stream_frame.h FORCE
	@mkdir -p $(@D)
	@$(STRUCTGEN) --cwd . $(SG_SF_ARGS) --build 'sf=$(strip $(INC) $(SG_SYSROOT))' --target $(SG_HOST_TRIPLE) --ts $@.ts && \
	  $(call gen_sync,$@.ts,$(SF_TS)) && touch $@

.PHONY: all test smoke check test-asan smoke-asan check-asan clean graph import fuzz fuzz-smoke
all: $(WIRE_HASH_FILE) $(B)/layout/stream_frame.stamp $(B)/adventurer $(B)/adv-import $(B)/adv-gen $(B)/adv-bench
import: $(B)/adv-import

# Every test program is its own process; they run CHECK_JOBS at a time, each to its own log,
# every one runs even when another fails, and the target names each that did.
CHECK_JOBS ?= $(shell getconf _NPROCESSORS_ONLN)
RUN_ALL_SH = mkdir -p $(B)/logs && printf '%s\0' $(2) | xargs -0 -P $(CHECK_JOBS) -n 1 sh -c \
  'log=$(B)/logs/$$(echo "$$0" | tr " /=" "___").log; if eval "$$0" > $$log 2>&1; then cat $$log; else cat $$log; echo "FAILED: $$0"; exit 1; fi' \
  || { echo "$(1) FAILED"; exit 1; }
define RUN_ALL
	@$(call RUN_ALL_SH,$(1),$(2))
endef
UNIT_RUNS  = $(addprefix $(1)/test/,$(UNITS))
SMOKE_RUNS = $(foreach s,$(SMOKES),'$(1)/test/$(s) $(1)/adventurer $(1)/adv-gen')
SMOKE_PROGS = $(1)/adventurer $(1)/adv-gen $(addprefix $(1)/test/,$(SMOKES))

test: $(call UNIT_RUNS,$(B))
	$(call RUN_ALL,UNIT TESTS,$(call UNIT_RUNS,$(B)))
smoke: $(call SMOKE_PROGS,$(B))
	$(call RUN_ALL,SMOKE,$(call SMOKE_RUNS,$(B)))
check: $(call UNIT_RUNS,$(B)) $(call SMOKE_PROGS,$(B))
	$(call RUN_ALL,CHECK,$(call SMOKE_RUNS,$(B)) $(call UNIT_RUNS,$(B)))
test-asan: $(call UNIT_RUNS,$(B)/asan)
	$(call RUN_ALL,ASAN UNIT TESTS,$(call UNIT_RUNS,$(B)/asan))
smoke-asan: $(call SMOKE_PROGS,$(B)/asan)
	$(call RUN_ALL,ASAN SMOKE,$(call SMOKE_RUNS,$(B)/asan))
check-asan: $(call UNIT_RUNS,$(B)/asan) $(call SMOKE_PROGS,$(B)/asan)
	$(call RUN_ALL,ASAN CHECK,$(call SMOKE_RUNS,$(B)/asan) $(call UNIT_RUNS,$(B)/asan))

# ---- the graph -------------------------------------------------------------------------------
DATA ?= ../data
PBF  ?= $(DATA)/region.osm.pbf
GRAPH ?= $(DATA)/region.graph
graph: $(GRAPH)
# THE REGION'S SHAPE: New York City's five boroughs with Nassau, Westchester, Hudson and Bergen
# around them (../tools/fetch_osm.sh cuts the extract to a box around those nine counties, and the
# fence keeps exactly them). Regions are the boroughs and NY's towns and cities (admin_level 7)
# with the villages (8) inside the towns, and NJ's municipalities (8); a road is its innermost's.
IMPORT_ARGS ?= --admin-levels 7,8 --within "6:New York County,Kings County,Queens County,Bronx County,Richmond County,Nassau County,Westchester County,Hudson County,Bergen County"
$(GRAPH): $(B)/adv-import $(PBF)
	$(B)/adv-import $(PBF) $@ $(IMPORT_ARGS)

# ---- fuzz (c_base/fuzz/fuzz.mk's shape, for adventurer's own targets) -------------------------
ifeq ($(UNAME),Darwin)
  FUZZ_CC ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang) clang)
else
  FUZZ_CC ?= clang
endif
FUZZ_TIME ?= 20
FUZZ_SAN := -fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined -fno-omit-frame-pointer
FUZZ_CFLAGS = $(filter-out -O2,$(CFLAGS)) -O1 $(FUZZ_SAN) -DCB_INSTRUMENTED -DADV_INSTRUMENTED
$(eval $(call C_VARIANT,$(B)/obj/fuzz,FUZZ_CC,FUZZ_CFLAGS))
FUZZ_SRC := $(sort $(wildcard fuzz/fz_*.c))
FUZZ_TARGETS := $(sort $(patsubst fz_%.c,%,$(notdir $(FUZZ_SRC))))
define FUZZ_PROG
$(B)/fuzz/$(1): $$(call c_objs,$(B)/obj/fuzz,fuzz/fz_$(1).c $$(LIB_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(FUZZ_CC) $$(FUZZ_CFLAGS) -fsanitize=fuzzer $$^ -o $$@ $$(LDLIBS)
endef
$(foreach t,$(FUZZ_TARGETS),$(eval $(call FUZZ_PROG,$(t))))
fuzz: $(addprefix $(B)/fuzz/,$(FUZZ_TARGETS))
fuzz-smoke: fuzz
	@set -e; for t in $(FUZZ_TARGETS); do \
	  mkdir -p $(B)/fuzz-corpus/$$t $(B)/fuzz-crashes/$$t fuzz/corpus/$$t; \
	  echo "fuzz $$t for $(FUZZ_TIME)s"; \
	  $(B)/fuzz/$$t -max_total_time=$(FUZZ_TIME) -max_len=65536 -artifact_prefix=$(B)/fuzz-crashes/$$t/ $(B)/fuzz-corpus/$$t fuzz/corpus/$$t 2> $(B)/fuzz-$$t.log || { tail -40 $(B)/fuzz-$$t.log; exit 1; }; \
	  tail -1 $(B)/fuzz-$$t.log; done

clean:
	rm -rf $(B)
