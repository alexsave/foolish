# structgen: one C file on libclang. LLVM must be the same one that builds the
# wasm (WASM_CC), so the layout libclang reports is the layout clang emits.
#
# THE FALLBACK IS A MAC FALLBACK, and it used to be everybody's. A Linux box
# with no llvm-config on PATH took /opt/homebrew/opt/llvm - a directory that
# cannot exist there - and the failure arrived as
#
#     structgen.c:100:10: fatal error: clang-c/Index.h: No such file or directory
#
# at the bottom of a `make wasm-bots-test` run, three layers below whatever the
# lane was actually doing; 26 validation scenarios reported it as their own
# failure and main was red for a day (#171). A default that names a path the
# platform cannot have is worse than no default: say so here instead.
UNAME_S := $(shell uname -s)
LLVM_PREFIX ?= $(shell llvm-config --prefix 2>/dev/null)
ifeq ($(strip $(LLVM_PREFIX)),)
  ifeq ($(UNAME_S),Darwin)
    LLVM_PREFIX := /opt/homebrew/opt/llvm
  endif
endif
ifeq ($(strip $(LLVM_PREFIX)),)
  $(error structgen needs libclang and found no llvm-config on PATH. Install it \
    and/or pass LLVM_PREFIX=<prefix>. Ubuntu: `apt-get install libclang-18-dev` \
    then LLVM_PREFIX=/usr/lib/llvm-18 (scripts/ci_llvm.sh does both). \
    macOS: `brew install llvm`)
endif
# -Werror=implicit-function-declaration: gcc 13 (Ubuntu 24.04, CI's cc) only WARNS
# on a call to an undeclared function, and assumes it returns int. glibc under
# -std=c11 does not declare strdup, so a strdup here returned a pointer cut to 32
# bits on x86_64 and structgen segfaulted on its first run (5eb22685). The CI
# structgen job builds with -Werror on top.
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -Werror=int-conversion

SRC ?= structgen.c
build/structgen: $(SRC)
	@mkdir -p build
	@[ -f "$(LLVM_PREFIX)/include/clang-c/Index.h" ] || { \
	  echo "structgen: no libclang headers under LLVM_PREFIX=$(LLVM_PREFIX)"; \
	  echo "structgen: (looked for $(LLVM_PREFIX)/include/clang-c/Index.h)"; \
	  echo "structgen: Ubuntu: bash scripts/ci_llvm.sh   macOS: brew install llvm"; \
	  exit 1; }
	$(CC) $(CFLAGS) -I$(LLVM_PREFIX)/include -DSG_RESOURCE_DIR='"$(shell $(LLVM_PREFIX)/bin/clang -print-resource-dir)"' $< -L$(LLVM_PREFIX)/lib -lclang -Wl,-rpath,$(LLVM_PREFIX)/lib -o $@

clean:
	rm -rf build
.PHONY: clean
