# structgen: one C file on libclang. LLVM must be the same one that builds the
# wasm (WASM_CC), so the layout libclang reports is the layout clang emits.
LLVM_PREFIX ?= $(shell llvm-config --prefix 2>/dev/null || echo /opt/homebrew/opt/llvm)
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

SRC ?= structgen.c
build/structgen: $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(LLVM_PREFIX)/include -DSG_RESOURCE_DIR='"$(shell $(LLVM_PREFIX)/bin/clang -print-resource-dir)"' $< -L$(LLVM_PREFIX)/lib -lclang -Wl,-rpath,$(LLVM_PREFIX)/lib -o $@

clean:
	rm -rf build
.PHONY: clean
