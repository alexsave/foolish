# The layout probe's C: the pattern and the judge (layout_probe.h, header-only,
# the same file the probe app compiles), its test, and the recompression sweep.
# `test` builds anywhere; `sweep` is macOS only, because ImageIO is the codec.
CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
.PHONY: all test sweep clean
all: test
build:
	mkdir -p build
build/layout_probe_test: layout_probe_test.c layout_probe.h ../../c/test/check.h | build
	$(CC) $(CFLAGS) layout_probe_test.c -o $@
test: build/layout_probe_test
	./build/layout_probe_test
build/sweep: sweep.c layout_probe.h | build
	$(CC) $(CFLAGS) sweep.c -o $@ -framework ImageIO -framework CoreGraphics -framework CoreFoundation
sweep: build/sweep
	./build/sweep
clean:
	rm -rf build

# The probe app, for a simulator. The project is generated into ios/build (see
# ios/project.yml for why), and xcodebuild finds the one project there, so
# nothing here spells its name.
.PHONY: app
app:
	mkdir -p ios/build
	cd ios && xcodegen generate --project build
	cd ios/build && xcodebuild -scheme LayoutProbeApp -configuration Debug \
		-destination 'generic/platform=iOS Simulator' -derivedDataPath dd build
