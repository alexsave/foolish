# shared/c's native-only tests that need a library the products do not link:
#
#   make -C shared/c openssl-tests    sha256.c and deal_rng.c against OpenSSL
#
# Both compare a hand-written primitive with OpenSSL's over random inputs, so OpenSSL is
# the independent oracle and is NOT part of any kernel. CI runs this on a lane that has it
# (.github/workflows/shared_openssl.yml: ubuntu with libssl-dev). OPENSSL=<prefix> names a
# prefix with include/ and lib/ (macOS: /opt/homebrew/opt/openssl@3); otherwise pkg-config.
CC     ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -ffp-contract=off -fno-math-errno
ifdef OPENSSL
  SSL_CFLAGS := -I$(OPENSSL)/include
  SSL_LIBS   := -L$(OPENSSL)/lib -lcrypto
else ifneq ($(shell pkg-config --exists libcrypto && echo y),)
  SSL_CFLAGS := $(shell pkg-config --cflags libcrypto)
  SSL_LIBS   := $(shell pkg-config --libs libcrypto)
else ifneq ($(wildcard /opt/homebrew/opt/openssl@3/include),)
  SSL_CFLAGS := -I/opt/homebrew/opt/openssl@3/include
  SSL_LIBS   := -L/opt/homebrew/opt/openssl@3/lib -lcrypto
else
  SSL_CFLAGS :=
  SSL_LIBS   := -lcrypto
endif

OPENSSL_TESTS := sha256_openssl_test deal_rng_openssl_test

.PHONY: openssl-tests clean
openssl-tests: $(addprefix build/,$(OPENSSL_TESTS))
	@for t in $(OPENSSL_TESTS); do echo "./build/$$t"; ./build/$$t || exit 1; done

build/sha256_openssl_test: sha256_openssl_test.c sha256.c sha256.h rng.h test/check.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SSL_CFLAGS) sha256_openssl_test.c sha256.c $(SSL_LIBS) -o $@
build/deal_rng_openssl_test: deal_rng_openssl_test.c deal_rng.c deal_rng.h rng.h test/check.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SSL_CFLAGS) deal_rng_openssl_test.c deal_rng.c $(SSL_LIBS) -o $@

clean:
	rm -rf build
