# c_chess: a chess website on c_base, built for speed (README.md, DESIGN.md).
#
#   make              the server (build/c_chess), the load generator (build/chessbench), the tests
#   make test         every kernel and server unit test (c/test/unit_*.c, server/test/unit_*.c)
#   make check        test, then the smoke (a real server played over its real socket)
#   make check-asan   the same, every object rebuilt under ASan + UBSan
#   make wasm         the browser's kernel: web/public/chess.wasm (WASM_CC: Homebrew LLVM)
#   make gen          the wire's TypeScript readers (structgen): web/src/gen/
#   make clean
#
# THE PLATFORM IS c_base's, BY SOURCE: every .c under c_base/core/src, providers/local/src and
# providers/aws/src is compiled here with this product's flags; no file of c_base's is copied.
# The kernel is c/src (no I/O, no allocation: the same objects run in the server, the load
# generator and, as wasm, the browser). The server is server/src.
#
# NO FILE LISTS TO EDIT: every unit_*.c under c/test and server/test is a test program, every
# lib_*.c under server/test is linked into the server's.

# NO BUILT-IN RULES: make tries to remake every included .d file, and its built-in implicit rules
# (RCS, SCCS, yacc, ...) only make that search over a thousand dependency files longer.
MAKEFLAGS += --no-builtin-rules
.SUFFIXES:

C_ROOT := ..
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

KERNEL_SRC := $(sort $(wildcard c/src/*.c))

PLATFORM_ROOTS := $(CB)/core/src $(CB)/providers/local/src $(CB)/providers/aws/src
PLATFORM_MAINS := $(CB)/providers/local/src/main_serve.c $(CB)/providers/aws/src/main_lambda.c
PLATFORM_SRC := $(sort $(filter-out $(PLATFORM_MAINS),$(foreach r,$(PLATFORM_ROOTS),$(wildcard $(r)/*.c $(r)/*/*.c))))
SHARED_SRC := $(C_ROOT)/shared/c/b64/b64.c $(C_ROOT)/shared/c/hex/hex.c $(C_ROOT)/shared/c/crc32c/crc32c.c \
              $(C_ROOT)/shared/c/mqtt/mqtt.c $(C_ROOT)/shared/c/sha256.c
GEN_SRC := build/gen/schema_sql.c build/gen/console_html.c build/gen/chess_schema_sql.c
APP_SRC := $(sort $(wildcard server/src/*.c))
SERVER_SRC := $(PLATFORM_SRC) $(SHARED_SRC) $(GEN_SRC) $(APP_SRC) $(KERNEL_SRC)

PLATFORM_INC := -I$(CB)/core/include -I$(CB)/core/src -I$(CB)/providers/local/src -I$(CB)/providers/aws/src -I$(C_ROOT)/shared/c
INC := $(PLATFORM_INC) -Ic/src -Iserver/src -Ibuild/gen -I$(CB)/core/test
CFLAGS = -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wvla \
         $(INC) $(SQLITE_CFLAGS) $(OPENSSL_CFLAGS) -pthread
LDLIBS := $(SQLITE_LDFLAGS) $(OPENSSL_LDFLAGS) -lsqlite3 -lssl -lcrypto -lz -lpthread -lm
ifeq ($(STATIC),1)
  LDLIBS += -static
endif

SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
ASAN_CFLAGS = $(filter-out -O2,$(CFLAGS)) -O1 $(SAN) -DCB_INSTRUMENTED

include $(C_ROOT)/shared/tools/c_rules.mk
$(eval $(call C_VARIANT,build/obj/rel,CC,CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,ASAN_CFLAGS))

build/embed: $(CB)/tools/embed.c
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@
build/gen/schema_sql.c: $(CB)/providers/local/sql/schema.sql build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_schema_sql > $@.new && mv $@.new $@
build/gen/console_html.c: $(CB)/core/src/admin/console.html build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_console_html > $@.new && mv $@.new $@
build/gen/chess_schema_sql.c: server/sql/schema.sql build/embed
	@mkdir -p $(@D)
	build/embed $< cc_schema_sql > $@.new && mv $@.new $@

KTEST_SRC := $(sort $(wildcard c/test/unit_*.c))
KTESTS    := $(sort $(basename $(notdir $(KTEST_SRC))))
STEST_SRC := $(sort $(wildcard server/test/unit_*.c))
STESTS    := $(sort $(basename $(notdir $(STEST_SRC))))
SMOKES    := $(sort $(basename $(notdir $(wildcard server/test/smoke*.c))))
TLIB_SRC  := $(sort $(wildcard server/test/lib_*.c)) $(CB)/core/test/lib_client.c $(CB)/core/test/lib_proc.c

define PROG
$(1)/$(3): $$(call c_objs,$(2),$(4))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(CC) $$($(5)) $$^ -o $$@ $$(LDLIBS)
endef
prog = $(eval $(call PROG,$(1),$(2),$(3),$(4),$(5)))

$(call prog,build,build/obj/rel,c_chess,server/tools/main_serve.c $(SERVER_SRC),CFLAGS)
$(call prog,build,build/obj/rel,chessbench,server/tools/chessbench.c $(SERVER_SRC),CFLAGS)
$(call prog,build,build/obj/rel,puzzlegen,server/tools/puzzlegen.c $(SERVER_SRC),CFLAGS)
$(call prog,build/asan,build/obj/asan,c_chess,server/tools/main_serve.c $(SERVER_SRC),ASAN_CFLAGS)
$(call prog,build/asan,build/obj/asan,chessbench,server/tools/chessbench.c $(SERVER_SRC),ASAN_CFLAGS)
$(foreach u,$(KTESTS),$(call prog,build/test,build/obj/rel,$(u),c/test/$(u).c $(KERNEL_SRC),CFLAGS))
$(foreach u,$(KTESTS),$(call prog,build/asan/test,build/obj/asan,$(u),c/test/$(u).c $(KERNEL_SRC),ASAN_CFLAGS))
$(foreach u,$(STESTS) $(SMOKES),$(call prog,build/test,build/obj/rel,$(u),server/test/$(u).c $(TLIB_SRC) $(SERVER_SRC),CFLAGS))
$(foreach u,$(STESTS) $(SMOKES),$(call prog,build/asan/test,build/obj/asan,$(u),server/test/$(u).c $(TLIB_SRC) $(SERVER_SRC),ASAN_CFLAGS))

.PHONY: all test smoke check test-asan check-asan kernel-test clean wasm gen
all: build/c_chess build/chessbench build/puzzlegen

CHECK_JOBS ?= 4
RUN_ALL_SH = mkdir -p build/logs && printf '%s\0' $(2) | xargs -0 -P $(CHECK_JOBS) -n 1 sh -c \
  'log=build/logs/$$(echo "$$0" | tr " /=" "___").log; if eval "$$0" > $$log 2>&1; then tail -1 $$log; else cat $$log; echo "FAILED: $$0"; exit 1; fi' \
  || { echo "$(1) FAILED"; exit 1; }
define RUN_ALL
	@$(call RUN_ALL_SH,$(1),$(2))
endef
UNIT_RUNS  = $(addprefix $(1)/test/,$(KTESTS) $(STESTS))
SMOKE_RUNS = $(foreach s,$(SMOKES),'$(1)/test/$(s) $(1)/c_chess $(1)/chessbench')
SMOKE_PROGS = $(1)/c_chess $(1)/chessbench $(addprefix $(1)/test/,$(SMOKES))

kernel-test: $(addprefix build/test/,$(KTESTS))
	$(call RUN_ALL,KERNEL TESTS,$(addprefix build/test/,$(KTESTS)))
test: $(call UNIT_RUNS,build)
	$(call RUN_ALL,UNIT TESTS,$(call UNIT_RUNS,build))
smoke: $(call SMOKE_PROGS,build)
	$(call RUN_ALL,SMOKE,$(call SMOKE_RUNS,build))
check: $(call UNIT_RUNS,build) $(call SMOKE_PROGS,build)
	$(call RUN_ALL,CHECK,$(call SMOKE_RUNS,build) $(call UNIT_RUNS,build))
test-asan: $(call UNIT_RUNS,build/asan)
	$(call RUN_ALL,ASAN UNIT TESTS,$(call UNIT_RUNS,build/asan))
check-asan: $(call UNIT_RUNS,build/asan) $(call SMOKE_PROGS,build/asan)
	$(call RUN_ALL,ASAN CHECK,$(call SMOKE_RUNS,build/asan) $(call UNIT_RUNS,build/asan))

# ---- the browser's kernel (c/wasm/web.c) and the page's readers of its structs ----
# The MVP feature set plus bulk memory and sign extension (-mcpu=mvp: an older Safari refuses more),
# -O3 for the move generator; the module is stamped with the layout's hash, which the page checks.
include $(C_ROOT)/shared/tools/gen_tools.mk
include $(C_ROOT)/shared/tools/wasm_cc.mk
WEB_FLAGS   := -isystem $(C_ROOT)/shared/c/wasm/include -Ic/src -Ic/wasm -I$(C_ROOT)/shared/c
WEB_SG_ARGS := --cwd . --header c/wasm/web.h --root CvView --root CvMoves --root CvLobby --root CvProfile --root CvGames --root CvTv --root CvTop --root CvEval --root CvPuzzle \
               --snapshot CvView --snapshot CvMoves --snapshot CvLobby --snapshot CvProfile --snapshot CvGames --snapshot CvTv --snapshot CvTop --snapshot CvEval --snapshot CvPuzzle --snapshot-only --const CV_ --const CC_EV_ --const CW_A_ --const CC_ --const CW_E_ --const CW_AUTH_ --const CH_ --const CP_ --build 'web=$(WEB_FLAGS)'
WEB_GEN     := web/src/gen
WEB_WASM_SRC := c/src/chess.c c/src/game.c c/src/client.c c/src/puzzle.c c/wasm/web.c $(C_ROOT)/shared/c/wasm/libc.c
WEB_WASM    := web/public/chess.wasm

gen: $(STRUCTGEN)
	@mkdir -p build $(WEB_GEN)
	@$(STRUCTGEN) $(WEB_SG_ARGS) --ts $(WEB_GEN)/web_layout.ts --hash-ts $(WEB_GEN)/web_layout_hash.ts --print-hash > build/web_layout.hash

wasm: gen
	@mkdir -p $(dir $(WEB_WASM))
	$(WASM_CC) --target=wasm32 -mcpu=mvp -mbulk-memory -msign-ext -O3 -nostdlib -ffreestanding \
	  -mno-nontrapping-fptoint -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter $(WEB_FLAGS) \
	  -DSG_LAYOUT_HASH=$$(cat build/web_layout.hash) \
	  -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=262144 -Wl,--strip-all \
	  $(WEB_WASM_SRC) -o $(WEB_WASM)
	@echo "built $(WEB_WASM) ($$(wc -c < $(WEB_WASM)) B, $$(gzip -9 -c $(WEB_WASM) | wc -c) B gz), layout $$(cat build/web_layout.hash)"

clean:
	rm -rf build
