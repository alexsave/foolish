# The product byte-decoder fuzz targets: forwarded bubbles are untrusted input,
# so every reader of a product's message bytes gets a libFuzzer target here.
# c_base's own targets live in c_base/fuzz (c_base/Makefile); these are built
# from the products' kernels, which c_base's Makefile does not link, so this
# directory has its own tiny Makefile and its own nightly workflow
# (.github/workflows/product_fuzz_nightly.yml).
#
#   make seeds           write each target's seeds (from the products' real
#                        encoders) into corpus/<target>
#   make build/<target>  libFuzzer + ASan + UBSan binary (clang with libFuzzer;
#                        Homebrew LLVM on macOS)
#   make smoke           every target for FUZZ_TIME seconds (default 30) on a
#                        scratch copy of its corpus; crashes land in
#                        build/crashes/<target>
#   make nightly         the same with FUZZ_TIME=600
#   make print-targets
#
# A target is fz_<name>.c plus a TARGET_SRC_<name> line below.
ROOT := ../..
SH   := $(ROOT)/shared/c
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  FUZZ_CC ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang) clang)
else
  FUZZ_CC ?= clang
endif
FUZZ_TIME ?= 30
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
CFLAGS := -O1 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -ffp-contract=off $(SAN) \
          -I$(SH) -I$(ROOT)/pickemup/c/src -I$(ROOT)/tallybones/c/src -I$(ROOT)/chuiniu/c/src \
          -I$(ROOT)/uttt/c/src -I$(ROOT)/foolish/c/src

WHOLE := $(SH)/deal_rng.c $(SH)/sha256.c $(SH)/b32.c $(SH)/mixrad.c $(SH)/text_util/text_util.c \
         $(SH)/wire_check/wire_check.c $(SH)/msg_seat_tag/msg_seat_tag.c \
         $(SH)/msg_lobby_roster/msg_lobby_roster.c $(SH)/msg_whole/msg_whole.c

PK_SRC := $(addprefix $(ROOT)/pickemup/c/src/,pk.c pk_deck.c pk_plan.c pk_view.c pk_lobby.c pk_say.c pk_code.c pk_msg.c pk_beats.c pk_arrange.c) \
          $(ROOT)/pickemup/c/i18n/strings_en.c $(WHOLE)
TB_SRC := $(addprefix $(ROOT)/tallybones/c/src/,tb.c tb_code.c tb_plan.c tb_view.c tb_lobby.c tb_msg.c tb_say.c tb_beats.c) \
          $(ROOT)/tallybones/c/i18n/strings_en.c $(WHOLE)
CN_SRC := $(addprefix $(ROOT)/chuiniu/c/src/,cn.c cn_dice.c cn_plan.c cn_view.c cn_lobby.c cn_say.c cn_code.c cn_msg.c cn_beats.c cn_roll.c cn_libm.c cn_scene.c cn_geom.c cn_cam.c cn_lay.c cn_tex.c cn_stage.c) \
          $(ROOT)/chuiniu/c/i18n/strings_en.c $(WHOLE)
UTTT_SRC := $(addprefix $(ROOT)/uttt/c/src/,uttt.c uttt_code.c uttt_msg.c uttt_big.c uttt_big_msg.c uttt_big_diag.c uttt_rule.c uttt_say.c uttt_pen.c uttt_draw.c uttt_anim.c uttt_bots.c uttt_lang.c) $(wildcard $(ROOT)/uttt/c/i18n/strings_*.c) \
          $(SH)/sha256.c $(SH)/b32.c $(SH)/mixrad.c $(SH)/text_util/text_util.c $(SH)/wire_check/wire_check.c
FOOLISH_SRC := $(addprefix $(ROOT)/foolish/c/src/,game.c legal.c replay.c replay_steps.c replay_extras.c view.c awire.c evwire.c msg_wire.c card.c) \
          $(SH)/deal_rng.c $(SH)/b32.c $(SH)/sha256.c

TARGETS := msg_whole pickemup tallybones chuiniu uttt foolish_wire
SRC_msg_whole    := $(PK_SRC)
SRC_pickemup     := $(PK_SRC)
SRC_tallybones   := $(TB_SRC)
SRC_chuiniu      := $(CN_SRC)
SRC_uttt         := $(UTTT_SRC)
SRC_foolish_wire := $(FOOLISH_SRC)

.PHONY: all seeds smoke nightly print-targets clean
all: $(addprefix build/,$(TARGETS))
print-targets:
	@echo $(TARGETS)

define PROG
build/$(1): fz_$(1).c fz.h $$(SRC_$(1))
	@mkdir -p build
	$$(FUZZ_CC) $$(CFLAGS) -fsanitize=fuzzer fz_$(1).c $$(SRC_$(1)) -o $$@ -lm
build/seeds_$(1): fz_$(1).c fz.h $$(SRC_$(1))
	@mkdir -p build
	$$(FUZZ_CC) $$(CFLAGS) -DFZ_SEEDS fz_$(1).c $$(SRC_$(1)) -o $$@ -lm
endef
$(foreach t,$(TARGETS),$(eval $(call PROG,$(t))))

seeds: $(addprefix build/seeds_,$(TARGETS))
	@for t in $(TARGETS); do mkdir -p corpus/$$t && build/seeds_$$t corpus/$$t || exit 1; done

smoke: all
	@set -e; for t in $(TARGETS); do \
	  rm -rf build/work/$$t build/crashes/$$t; mkdir -p build/work/$$t build/crashes/$$t; \
	  cp corpus/$$t/* build/work/$$t/ 2>/dev/null || true; \
	  echo "== $$t ($(FUZZ_TIME) s)"; \
	  build/$$t -max_total_time=$(FUZZ_TIME) -artifact_prefix=build/crashes/$$t/ build/work/$$t; \
	done
nightly:
	@$(MAKE) smoke FUZZ_TIME=600
clean:
	rm -rf build
