# lichess_ws/server: lila-ws, lichess's WebSocket gateway, rebuilt on c_base (../SCOPE.md section 3).
#
#   make              build/lws (`lws serve`), build/wsbench (the load generator with its fake lila)
#   make test         every unit test (test/unit_*.c), each its own program
#   make check        test, then the smoke (a real server, a real Redis, a real MongoDB)
#   make check-asan   the same, every object rebuilt under ASan + UBSan
#   make clean
#
# THE PLATFORM IS c_base's, BY SOURCE, as adventurer/server/Makefile does it: every .c under
# c_base/core/src, c_base/providers/local/src and c_base/providers/aws/src is compiled here with
# this product's flags, and no file of c_base's is copied or edited. The product is every .c
# under src/.
#
# NO FILE LISTS TO EDIT: every unit_*.c under test/ is a test program, every lib_*.c there is
# linked into each.

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
GEN_SRC := build/gen/schema_sql.c build/gen/console_html.c
APP_SRC := $(sort $(wildcard src/*.c))
LIB_SRC := $(PLATFORM_SRC) $(SHARED_SRC) $(GEN_SRC) $(APP_SRC)

PLATFORM_INC := -I$(CB)/core/include -I$(CB)/core/src -I$(CB)/providers/local/src -I$(CB)/providers/aws/src -I$(C_ROOT)/shared/c
INC := $(PLATFORM_INC) -Isrc -Itest -I$(CB)/core/test
CFLAGS = -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wvla \
         $(INC) $(SQLITE_CFLAGS) $(OPENSSL_CFLAGS) -pthread
LDLIBS := $(SQLITE_LDFLAGS) $(OPENSSL_LDFLAGS) -lsqlite3 -lssl -lcrypto -lz -lpthread -lm
# STATIC=1: a statically linked binary (tools/build_linux.sh links one on Alpine, musl, for the
# lichess mono container and for a Linux host).
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

TEST_SRC := $(sort $(wildcard test/*.c))
TLIB_SRC := $(sort $(wildcard test/lib_*.c)) $(CB)/core/test/lib_client.c $(CB)/core/test/lib_proc.c
UNIT_SRC := $(sort $(wildcard test/unit_*.c))
UNITS    := $(sort $(basename $(notdir $(UNIT_SRC))))
SMOKES   := $(sort $(basename $(notdir $(wildcard test/smoke*.c))))

define PROG
$(1)/$(3): $$(call c_objs,$(2),$(4) $$(LIB_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(CC) $$($(5)) $$^ -o $$@ $$(LDLIBS)
endef
prog = $(eval $(call PROG,$(1),$(2),$(3),$(4),$(5)))

$(call prog,build,build/obj/rel,lws,tools/main_serve.c,CFLAGS)
$(call prog,build,build/obj/rel,wsbench,tools/wsbench.c,CFLAGS)
$(call prog,build,build/obj/rel,dropin,tools/dropin.c $(TLIB_SRC),CFLAGS)
$(call prog,build/asan,build/obj/asan,lws,tools/main_serve.c,ASAN_CFLAGS)
$(call prog,build/asan,build/obj/asan,wsbench,tools/wsbench.c,ASAN_CFLAGS)
$(foreach u,$(UNITS) $(SMOKES),$(call prog,build/test,build/obj/rel,$(u),test/$(u).c $(TLIB_SRC),CFLAGS))
$(foreach u,$(UNITS) $(SMOKES),$(call prog,build/asan/test,build/obj/asan,$(u),test/$(u).c $(TLIB_SRC),ASAN_CFLAGS))

.PHONY: all test smoke check test-asan smoke-asan check-asan clean
all: build/lws build/wsbench

CHECK_JOBS ?= 4
RUN_ALL_SH = mkdir -p build/logs && printf '%s\0' $(2) | xargs -0 -P $(CHECK_JOBS) -n 1 sh -c \
  'log=build/logs/$$(echo "$$0" | tr " /=" "___").log; if eval "$$0" > $$log 2>&1; then cat $$log; else cat $$log; echo "FAILED: $$0"; exit 1; fi' \
  || { echo "$(1) FAILED"; exit 1; }
define RUN_ALL
	@$(call RUN_ALL_SH,$(1),$(2))
endef
UNIT_RUNS  = $(addprefix $(1)/test/,$(UNITS))
SMOKE_RUNS = $(foreach s,$(SMOKES),'$(1)/test/$(s) $(1)/lws $(1)/wsbench')
SMOKE_PROGS = $(1)/lws $(1)/wsbench $(addprefix $(1)/test/,$(SMOKES))

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

clean:
	rm -rf build
