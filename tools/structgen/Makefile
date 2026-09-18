# structgen: one C file on libclang. LLVM must be the same one that builds the
# wasm (WASM_CC), so the layout libclang reports is the layout clang emits.
#
# The libclang discovery, the CFLAGS and the build rule are in tools/llvm.mk,
# shared with tools/datagen - the other program in this repo built on the same
# library. structgen asks clang for layouts; datagen asks it for the contents of
# static tables. A machine that can build one can build the other.
include ../llvm.mk

structgen: build/structgen
.PHONY: structgen
.DEFAULT_GOAL := structgen
