# structgen: one C file on libclang. LLVM must be the same one that builds the
# wasm (WASM_CC), so the layout libclang reports is the layout clang emits.
LLVM_PREFIX ?= $(shell llvm-config --prefix 2>/dev/null || echo /opt/homebrew/opt/llvm)
# -Werror=implicit-function-declaration: gcc 13 (Ubuntu 24.04, CI's cc) only WARNS
# on a call to an undeclared function, and assumes it returns int. glibc under
# -std=c11 does not declare strdup, so a strdup here returned a pointer cut to 32
# bits on x86_64 and structgen segfaulted on its first run (5eb22685). The CI
# structgen job builds with -Werror on top.
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -Werror=int-conversion

SRC ?= structgen.c
build/structgen: $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(LLVM_PREFIX)/include -DSG_RESOURCE_DIR='"$(shell $(LLVM_PREFIX)/bin/clang -print-resource-dir)"' $< -L$(LLVM_PREFIX)/lib -lclang -Wl,-rpath,$(LLVM_PREFIX)/lib -o $@

clean:
	rm -rf build
.PHONY: clean
