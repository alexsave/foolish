// product_includes_aws.c - the product may not include a provider's header.
// c_base/Makefile's compiler-gate compiles this file in the flags a source of
// foolish/server/src gets, and it must fail on this include; with every root on the path
// it must compile, so the header is real. docs/PLATFORM.md 8.4, the compiler gate.
#include "aws/aws_call.h"
