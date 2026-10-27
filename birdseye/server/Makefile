# birdseye/server: the birds-eye view of foolish (../DESIGN.md).
#
#   make              build/birdseye (`birdseye serve`: the simulator, the aggregate and the site's
#                     surface in one process) and the generated web decoder (../web/src/gen/wire.ts)
#   make test         every unit test (test/unit_*.c), each its own program
#   make check        test, then the smoke (a real cbase serve, the sim playing games through it)
#   make check-asan   the same under ASan + UBSan
#   make clean
#
# THE PLATFORM IS c_base's, BY SOURCE, as adventurer links it: every .c under c_base/core/src,
# c_base/providers/local/src and c_base/providers/aws/src, compiled here, no file copied. THE KERNEL
# is foolish's, by its own source list (foolish/c/Makefile print-kernel), with the human-game
# capacities the shipped builds use (print-human-caps), exactly as c_base compiles it: birdseye's
# fake humans read casts and choose moves with the kernel's client table and legal moves, and must
# agree with the server on every struct size. The product is every .c under src/.
#
# THE WEB'S DECODER is structgen's (shared/tools/structgen, libclang): it reads src/wire.h with this
# build's flags and writes ../web/src/gen/wire.ts and the layout hash, which is compiled into the
# server as BIRDSEYE_WIRE_HASH. No TypeScript spells a byte offset.

C_ROOT := ../..
CB     := $(C_ROOT)/c_base
KDIR   := $(C_ROOT)/foolish/c
UNAME  := $(shell uname)
.DEFAULT_GOAL := all

ifeq ($(UNAME),Darwin)
  OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
  SQLITE_PREFIX ?= $(shell brew --prefix sqlite 2>/dev/null)
  ifeq ($(wildcard $(SQLITE_PREFIX)/include/sqlite3.h),)
    $(error no Homebrew sqlite (SQLITE_PREFIX='$(SQLITE_PREFIX)'): brew install sqlite)
  endif
  PLATFORM_LDLIBS := -framework Accelerate
endif
ifneq ($(strip $(OPENSSL_PREFIX)),)
  OPENSSL_CFLAGS := -I$(OPENSSL_PREFIX)/include
  OPENSSL_LDFLAGS := -L$(OPENSSL_PREFIX)/lib
endif
ifneq ($(strip $(SQLITE_PREFIX)),)
  SQLITE_CFLAGS := -I$(SQLITE_PREFIX)/include
  SQLITE_LDFLAGS := -L$(SQLITE_PREFIX)/lib
endif

# ---- the kernel, exactly as c_base/Makefile reads it ------------------------------------------
KERNEL_LIST := $(shell MAKEFLAGS= $(MAKE) -s --no-print-directory -C $(KDIR) print-kernel)
ifeq ($(strip $(KERNEL_LIST)),)
  $(error could not read the kernel's source list (make -C $(KDIR) print-kernel))
endif
KERNEL_SRC := $(patsubst ../../%,$(C_ROOT)/%,$(filter ../../%,$(KERNEL_LIST))) \
              $(addprefix $(KDIR)/,$(filter-out ../../%,$(KERNEL_LIST)))
KERNEL_CAPS := $(shell MAKEFLAGS= $(MAKE) -s --no-print-directory -C $(KDIR) print-human-caps)
ifeq ($(filter -DMAX_LOG_PAIRS=%,$(KERNEL_CAPS)),)
  $(error could not read the kernel's human-game capacities (make -C $(KDIR) print-human-caps))
endif
KERNEL_CFLAGS := -O3 -ffp-contract=off -fno-math-errno -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
                 -I$(KDIR)/src -I$(C_ROOT)/shared/c -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK $(KERNEL_CAPS)

# ---- the platform and the product --------------------------------------------------------------
PLATFORM_ROOTS := $(CB)/core/src $(CB)/providers/local/src $(CB)/providers/aws/src
PLATFORM_MAINS := $(CB)/providers/local/src/main_serve.c $(CB)/providers/aws/src/main_lambda.c
PLATFORM_SRC := $(sort $(filter-out $(PLATFORM_MAINS),$(foreach r,$(PLATFORM_ROOTS),$(wildcard $(r)/*.c $(r)/*/*.c))))
# The shared helpers the platform needs that the kernel's own list does not already bring.
SHARED_SRC := $(filter-out $(KERNEL_SRC),$(C_ROOT)/shared/c/b64/b64.c $(C_ROOT)/shared/c/hex/hex.c $(C_ROOT)/shared/c/crc32c/crc32c.c $(C_ROOT)/shared/c/mqtt/mqtt.c $(C_ROOT)/shared/c/sha256.c \
              $(C_ROOT)/shared/c/stats/hist.c $(C_ROOT)/shared/c/proc.c)
GEN_SRC := build/gen/schema_sql.c build/gen/console_html.c
# The tests' blocking HTTP and WebSocket client (c_base/core/test): the sim's sign-up and its sockets.
CLIENT_SRC := $(CB)/core/test/lib_client.c
# The game id rule (store/game_id.h), the server's own: wire.h sizes a game id by it, and a
# GET /be/table/<gid> is checked by it.
APP_SRC := $(sort $(wildcard src/*.c)) $(C_ROOT)/foolish/server/src/store/game_id.c
LIB_SRC := $(PLATFORM_SRC) $(SHARED_SRC) $(GEN_SRC) $(CLIENT_SRC) $(APP_SRC)

PLATFORM_INC := -I$(CB)/core/include -I$(CB)/client/include -I$(CB)/core/src -I$(CB)/providers/local/src -I$(CB)/providers/aws/src -I$(C_ROOT)/shared/c
INC := $(PLATFORM_INC) -I$(C_ROOT)/foolish/server/src -I$(CB)/core/test -I$(KDIR)/src -Isrc -Itest
WIRE_HASH_FILE := build/gen/wire.hash
# The product's flags carry the kernel's capacities (struct sizes) and the wire hash (read from the
# file structgen wrote, which the variant's flag stamp names so a new hash is a new command line).
CFLAGS = -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wvla \
         $(INC) $(SQLITE_CFLAGS) $(OPENSSL_CFLAGS) -pthread $(KERNEL_CAPS) -DBIRDSEYE_WIRE_HASH=$$(cat $(WIRE_HASH_FILE))
LDLIBS := $(SQLITE_LDFLAGS) $(OPENSSL_LDFLAGS) -lsqlite3 -lssl -lcrypto -lz -lpthread -lm $(PLATFORM_LDLIBS)

SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
ASAN_CFLAGS = $(filter-out -O2,$(CFLAGS)) -O1 $(SAN) -DCB_INSTRUMENTED
ASAN_KERNEL_CFLAGS = $(filter-out -O3,$(KERNEL_CFLAGS)) -O1 $(SAN)

include $(C_ROOT)/shared/tools/c_rules.mk
$(eval $(call C_VARIANT,build/obj/rel,CC,CFLAGS,$(WIRE_HASH_FILE)))
$(eval $(call C_VARIANT,build/obj/kernel,CC,KERNEL_CFLAGS))
$(eval $(call C_VARIANT,build/obj/asan,CC,ASAN_CFLAGS,$(WIRE_HASH_FILE)))
$(eval $(call C_VARIANT,build/obj/asan-kernel,CC,ASAN_KERNEL_CFLAGS))

# The platform's schema and console, embedded as c_base embeds them.
build/embed: $(CB)/tools/embed.c
	@mkdir -p $(@D)
	$(CC) -O2 -Wall -Wextra -Werror $< -o $@
build/gen/schema_sql.c: $(CB)/providers/local/sql/schema.sql build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_schema_sql > $@.new && mv $@.new $@
build/gen/console_html.c: $(CB)/core/src/admin/console.html build/embed
	@mkdir -p $(@D)
	build/embed $< cbase_console_html > $@.new && mv $@.new $@

# ---- the web's decoder, by structgen -----------------------------------------------------------
STRUCTGEN := $(C_ROOT)/shared/tools/structgen/build/structgen
WIRE_TS   := ../web/src/gen/wire.ts
SG_ARGS := --cwd src --header wire.h --build 'be=-std=gnu11 -I../../../foolish/server/src' \
           --root be_w_card --root be_w_seat --root be_w_table --root be_w_event --root be_w_hand --root be_w_detail --root be_w_list --root be_w_session \
           --root be_w_bot --root be_w_result --root be_w_league --snapshot be_w_bot --snapshot be_w_result --snapshot be_w_league \
           --root be_w_match --root be_w_tour --snapshot be_w_match --snapshot be_w_tour --count be_w_match.gid=gid_len \
           --root be_w_rung --root be_w_gauntlet --snapshot be_w_rung --snapshot be_w_gauntlet --count be_w_rung.gid=gid_len \
           --count be_w_bot.id=id_len --count be_w_bot.name=name_len --count be_w_bot.key=key_len --count be_w_result.gid=gid_len \
           --count be_w_result.bot=n --count be_w_result.before=n --count be_w_result.after=n --count be_w_result.delta=n \
           --snapshot be_w_card --snapshot be_w_seat --snapshot be_w_table --snapshot be_w_event --snapshot be_w_hand --snapshot be_w_detail \
           --snapshot be_w_list --snapshot be_w_session --snapshot-only \
           --count be_w_seat.name=name_len --count be_w_seat.brain=brain_len --count be_w_table.gid=gid_len \
           --count be_w_table.attack=num_battles --count be_w_table.defense=num_battles --count be_w_table.seats=num_players \
           --count be_w_event.gid=gid_len --count be_w_event.cards=n_cards --count be_w_hand.cards=n --count be_w_detail.events=n_events --count be_w_session.token=token_len \
           --const BE_
$(STRUCTGEN):
	$(MAKE) -C $(C_ROOT)/shared/tools/structgen build/structgen
# One run writes both: the decoder (to build/gen, copied to the web) and the hash the server is
# compiled with. The hash file is written only when it changes, so an unchanged layout rebuilds
# nothing; the decoder is one target with the hash, never two recipes racing under -j.
$(WIRE_HASH_FILE): src/wire.h $(C_ROOT)/foolish/server/src/store/game_id.h $(STRUCTGEN)
	@mkdir -p $(dir $(WIRE_HASH_FILE))
	$(STRUCTGEN) $(SG_ARGS) --ts build/gen/wire.ts --hash-ts build/gen/wire_hash.ts --print-hash > $(WIRE_HASH_FILE).new
	@if cmp -s $(WIRE_HASH_FILE).new $(WIRE_HASH_FILE); then rm $(WIRE_HASH_FILE).new; else mv $(WIRE_HASH_FILE).new $(WIRE_HASH_FILE); fi
$(WIRE_TS): $(WIRE_HASH_FILE)
	@mkdir -p $(@D)
	cp build/gen/wire.ts $@
	cp build/gen/wire_hash.ts $(dir $@)wire_hash.ts

# The platform's metrics report (c_base/core/src/metrics.h), read by the site with a reader
# structgen writes from the platform's own header: the rows are found by name (be_metrics.h).
METRICS_TS := ../web/src/gen/metrics.ts
$(METRICS_TS): $(CB)/core/src/metrics.h $(STRUCTGEN)
	@mkdir -p $(@D)
	$(STRUCTGEN) --cwd $(CB)/core/src --header metrics.h --build 'be=-std=gnu11 -I../include -I. -I../../../shared/c' \
	  --root cb_metrics_hdr --root cb_metric_row --snapshot cb_metrics_hdr --snapshot cb_metric_row \
	  --fields cb_metrics_hdr=version,n_rows,n_platform,row_size,at_ms,uptime_s,rss_mb,cpu_us,pid,SIZE \
	  --fields cb_metric_row=kind,name_len,index,total,per_s,p50,p90,p99,max,name,SIZE --count cb_metric_row.name=name_len \
	  --const CB_METRIC_ --const CB_METRICS_WIRE --const CB_METRICS_N_ --ts $@.new && mv $@.new $@

# ---- the site's stream codec, in wasm ----------------------------------------------------------
# wasm/sf_wasm.c wraps shared/c/stream_frame.h's own writers and parser (the one owner of the
# socket's bytes) for the browser; structgen types its exports from the same link line and
# --check-wasm holds the built module to them. The toolchain is foolish/c's (Homebrew LLVM on macOS).
ifeq ($(origin WASM_CC),undefined)
  ifneq ($(wildcard /opt/homebrew/opt/llvm/bin/clang),)
    WASM_CC := /opt/homebrew/opt/llvm/bin/clang
  else
    WASM_CC := clang
  endif
endif
SF_WASM     := ../web/public/sf.wasm
SF_WASM_TS  := ../web/src/gen/sf_wasm.ts
SF_SRC      := wasm/sf_wasm.c $(C_ROOT)/shared/c/wasm/libc.c
SF_CFLAGS   := --target=wasm32 -Oz -nostdlib -ffreestanding -mbulk-memory -mno-nontrapping-fptoint \
               -isystem $(C_ROOT)/shared/c/wasm/include -Wall -Wextra -Wno-unused-parameter
SF_LDFLAGS  := -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=65536 -Wl,--stack-first -Wl,--strip-all \
               -Wl,--export=sfw_in -Wl,--export=sfw_in_cap -Wl,--export=sfw_out -Wl,--export=sfw_hello -Wl,--export=sfw_sub \
               -Wl,--export=sfw_ping -Wl,--export=sfw_kind -Wl,--export=sfw_reply_id -Wl,--export=sfw_reply_status \
               -Wl,--export=sfw_push_kind -Wl,--export=sfw_push_body
$(SF_WASM): $(SF_SRC) $(C_ROOT)/shared/c/stream_frame.h
	@mkdir -p $(@D)
	$(WASM_CC) $(SF_CFLAGS) $(SF_LDFLAGS) $(SF_SRC) -o $@
$(SF_WASM_TS): $(SF_WASM) $(STRUCTGEN)
	@mkdir -p $(@D)
	$(STRUCTGEN) --cwd . --source wasm/sf_wasm.c --build 'sf=$(SF_CFLAGS)' --target wasm32 \
	  --link '$(SF_LDFLAGS)' --exports-name SfWasm --exports-ts $@.new --check-wasm $(SF_WASM) && mv $@.new $@

TEST_SRC := $(sort $(wildcard test/*.c))
TLIB_SRC := $(sort $(wildcard test/lib_*.c)) $(CB)/core/test/lib_proc.c
UNIT_SRC := $(sort $(wildcard test/unit_*.c))
UNITS    := $(sort $(basename $(notdir $(UNIT_SRC))))
SMOKES   := $(sort $(basename $(notdir $(wildcard test/smoke*.c))))

# $(call prog,<outdir>,<variant>,<kernel variant>,<name>,<sources>,<flags var>)
define PROG
$(1)/$(4): $$(call c_objs,$(2),$(5) $$(LIB_SRC)) $$(call c_objs,$(3),$$(KERNEL_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(CC) $$($(6)) $$^ -o $$@ $$(LDLIBS)
endef
prog = $(eval $(call PROG,$(1),$(2),$(3),$(4),$(5),$(6)))

$(call prog,build,build/obj/rel,build/obj/kernel,birdseye,tools/main_serve.c,CFLAGS)
$(call prog,build/asan,build/obj/asan,build/obj/asan-kernel,birdseye,tools/main_serve.c,ASAN_CFLAGS)
$(foreach u,$(UNITS) $(SMOKES),$(call prog,build/test,build/obj/rel,build/obj/kernel,$(u),test/$(u).c $(TLIB_SRC),CFLAGS))
$(foreach u,$(UNITS) $(SMOKES),$(call prog,build/asan/test,build/obj/asan,build/obj/asan-kernel,$(u),test/$(u).c $(TLIB_SRC),ASAN_CFLAGS))

# Every object of the product and the tests needs the hash file first (its flags read it).
$(call c_objs,build/obj/rel,$(APP_SRC) tools/main_serve.c $(TEST_SRC)): | $(WIRE_HASH_FILE)
$(call c_objs,build/obj/asan,$(APP_SRC) tools/main_serve.c $(TEST_SRC)): | $(WIRE_HASH_FILE)

.PHONY: all test smoke check test-asan smoke-asan check-asan clean
all: build/birdseye $(WIRE_TS) $(METRICS_TS) $(SF_WASM) $(SF_WASM_TS)

CHECK_JOBS ?= 4
RUN_ALL_SH = mkdir -p build/logs && printf '%s\0' $(2) | xargs -0 -P $(CHECK_JOBS) -n 1 sh -c \
  'log=build/logs/$$(echo "$$0" | tr " /=" "___").log; if eval "$$0" > $$log 2>&1; then cat $$log; else cat $$log; echo "FAILED: $$0"; exit 1; fi' \
  || { echo "$(1) FAILED"; exit 1; }
define RUN_ALL
	@$(call RUN_ALL_SH,$(1),$(2))
endef
UNIT_RUNS  = $(addprefix $(1)/test/,$(UNITS))
# The smoke needs the game server too: c_base's own build (make -C c_base build/cbase).
SMOKE_RUNS = $(foreach s,$(SMOKES),'$(1)/test/$(s) $(1)/birdseye $(CB)/build/cbase')
SMOKE_PROGS = $(1)/birdseye $(addprefix $(1)/test/,$(SMOKES))

$(CB)/build/cbase:
	$(MAKE) -C $(CB) build/cbase

test: $(call UNIT_RUNS,build)
	$(call RUN_ALL,UNIT TESTS,$(call UNIT_RUNS,build))
smoke: $(call SMOKE_PROGS,build) $(CB)/build/cbase
	$(call RUN_ALL,SMOKE,$(call SMOKE_RUNS,build))
check: $(call UNIT_RUNS,build) $(call SMOKE_PROGS,build) $(CB)/build/cbase
	$(call RUN_ALL,CHECK,$(call SMOKE_RUNS,build) $(call UNIT_RUNS,build))
test-asan: $(call UNIT_RUNS,build/asan)
	$(call RUN_ALL,ASAN UNIT TESTS,$(call UNIT_RUNS,build/asan))
smoke-asan: $(call SMOKE_PROGS,build/asan) $(CB)/build/cbase
	$(call RUN_ALL,ASAN SMOKE,$(call SMOKE_RUNS,build/asan))
check-asan: $(call UNIT_RUNS,build/asan) $(call SMOKE_PROGS,build/asan) $(CB)/build/cbase
	$(call RUN_ALL,ASAN CHECK,$(call SMOKE_RUNS,build/asan) $(call UNIT_RUNS,build/asan))

clean:
	rm -rf build
