# BubbleDataKit: a board of three-state cells as a picture, and back.
#
#   make test   the C test (bubble_data_test.c, -Werror), then `swift test`
#   make ios    prove the package, Messages helpers included, builds for iOS
#
# Everything is built under build/: the C test, SwiftPM's scratch directory
# and xcodebuild's derived data. One directory to ignore, and one the shared/
# validation walk already skips (it reads every file under shared/ for a
# product name, and build products carry absolute paths).
CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
SRC := Sources/CBubbleData/bubble_data.c
HDR := Sources/CBubbleData/include/bubble_data.h

.PHONY: all test ctest swifttest ios clean
all: test

build:
	mkdir -p build

build/bubble_data_test: Tests/c/bubble_data_test.c $(SRC) $(HDR) ../../c/test/check.h | build
	$(CC) $(CFLAGS) Tests/c/bubble_data_test.c $(SRC) -o $@

build/bubble_data_asan: Tests/c/bubble_data_test.c $(SRC) $(HDR) ../../c/test/check.h | build
	$(CC) -g -O1 -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
		-fno-sanitize-recover=all Tests/c/bubble_data_test.c $(SRC) -o $@

# The C test twice: as it ships (-O2), and under the address and undefined
# behaviour sanitizers, which is what makes "no out-of-bounds read" a check.
ctest: build/bubble_data_test build/bubble_data_asan
	./build/bubble_data_test
	./build/bubble_data_asan

swifttest:
	swift test --scratch-path build/swift

test: ctest swifttest

ios:
	xcodebuild -scheme BubbleDataKit -destination 'generic/platform=iOS Simulator' \
		-derivedDataPath build/xcode build

clean:
	rm -rf build
