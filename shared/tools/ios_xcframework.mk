# The iOS xcframework: one static library, built for the device and for both
# simulator architectures, wrapped with its headers. Included by each product's
# c/Makefile AFTER c_rules.mk and after it has set what the slices are built
# with:
#
#     IOS_MIN_VERSION := 17.0
#     IOS_CFLAGS      := -O2 -Isrc -Iios/include ...
#     include ../../shared/tools/ios_xcframework.mk
#     $(eval $(call IOS_XCFRAMEWORK,name,$(IOS_SRC),ios/include,../ios/vendor/Name.xcframework))
#     ios-lib: $(IOS_XCFRAMEWORKS)
#
#   $(1) the library name: the slices are build/ios/<slice>/lib$(1).a
#   $(2) the C sources, each compiled once into every slice
#   $(3) the headers directory (the public header and its module.modulemap)
#   $(4) the xcframework to write, replaced whole
#   $(5) optional: what else the xcframework is made from, for a product that
#        assembles its headers directory instead of keeping one
#
# Everything lands under build/ios/ relative to the calling Makefile, and needs
# Xcode (xcrun, libtool, lipo, xcodebuild). The host-compiler twin of the same
# sources is each product's `ios-smoke`, which needs no Mac.
#
# `make ios-lib` DOES NOTHING WHEN NOTHING CHANGED, and that is what lets Xcode
# run it on every build (shared/scripts/xcode_kernel.sh) instead of a person
# remembering to. It used to be a recipe: delete every object, compile every
# source three times, rewrite the xcframework - correct, and too slow to hang
# on a build, so the app could link a kernel older than the C beside it and
# the one product that checked could only refuse and say "run make ios-lib".
# Now each slice is a variant (c_rules.mk): an object is recompiled when its
# source, a header it includes or its command line changed, a library is
# re-archived when one of its objects was, and the xcframework is rewritten
# when a library or a header was. Xcode relinks exactly when that last file
# moves.
#
# THE LAYOUT HASH, for a product whose Swift reads the kernel's structs through
# generated readers (shared/tools/structgen). It names its spec before the
# include, and gen_tools.mk is included first:
#
#     IOS_SG_SPEC      := ios/layout.args
#     IOS_KERNEL_SWIFT := ../ios/Generated/NameKernel.swift
#
# build/layout/ios.hash is then the hash of the layout the three slices share,
# IOS_KERNEL_SWIFT the readers generated from the same headers in the same
# rule, and every slice is compiled with -DSG_LAYOUT_HASH=<that hash>, so the
# library and its readers cannot be from two different headers without the app
# saying so at launch.

IOS_SLICES := device sim-arm64 sim-x86_64
IOS_CC_device     = xcrun -sdk iphoneos clang -target arm64-apple-ios$(IOS_MIN_VERSION)
IOS_CC_sim-arm64  = xcrun -sdk iphonesimulator clang -target arm64-apple-ios$(IOS_MIN_VERSION)-simulator
IOS_CC_sim-x86_64 = xcrun -sdk iphonesimulator clang -target x86_64-apple-ios$(IOS_MIN_VERSION)-simulator

ifeq ($(strip $(IOS_SG_SPEC)),)
IOS_LAYOUT_HASH :=
IOS_LIB_CFLAGS   = $(IOS_CFLAGS)
else
IOS_LAYOUT_HASH := build/layout/ios.hash
IOS_LIB_CFLAGS   = $(IOS_CFLAGS) -DSG_LAYOUT_HASH=$$(cat $(IOS_LAYOUT_HASH))
endif

# One slice: its objects, as a variant. $(1)=slice, compiled by $(IOS_CC_$(1)).
# The layout hash's CONTENTS are part of each slice's command line, so the hash
# file is named to the flag stamp rather than being a prerequisite compared by
# timestamp.
define IOS_SLICE
$(call C_VARIANT,build/ios/$(1)/obj,IOS_CC_$(1),IOS_LIB_CFLAGS,$(IOS_LAYOUT_HASH))
endef
$(foreach s,$(IOS_SLICES),$(eval $(call IOS_SLICE,$(s))))

# One slice's library. $(1)=library name  $(2)=slice  $(3)=sources
# The objects are named, not globbed: the archive holds what the source list
# says, and an object left behind by a source that was removed (or MOVED to the
# other library of a product that has two, which then defined every symbol in
# it twice) is not in it. In the order the glob had, by basename, so an archive
# is the archive it was.
ios_by_basename = $(foreach o,$(sort $(notdir $(1:.c=.o))),$(filter %/$(o:.o=.c),$(1)))
define IOS_SLICE_LIB
build/ios/$(2)/lib$(1).a: $$(call c_objs,build/ios/$(2)/obj,$$(call ios_by_basename,$(3)))
	@rm -f $$@
	libtool -static -o $$@ $$^
endef

define IOS_XCFRAMEWORK
$(foreach s,$(IOS_SLICES),$(call IOS_SLICE_LIB,$(1),$(s),$(2))
)
# BOTH simulator architectures, because `-destination 'generic/platform=iOS
# Simulator'` builds both and a one-arch library fails the link with "ignoring
# file ... found architecture arm64, required architecture x86_64" - which
# reads like a missing symbol and is not.
build/ios/sim/lib$(1).a: build/ios/sim-arm64/lib$(1).a build/ios/sim-x86_64/lib$(1).a
	@mkdir -p $$(@D)
	lipo -create $$^ -output $$@

# Info.plist stands for the xcframework, which is a directory: it is the one
# file -create-xcframework always writes.
$(4)/Info.plist: build/ios/device/lib$(1).a build/ios/sim/lib$(1).a $$(shell find $(3) -type f 2>/dev/null) $(5)
	@rm -rf $(4)
	@mkdir -p $$(dir $(4))
	xcodebuild -create-xcframework \
	  -library build/ios/device/lib$(1).a -headers $(3) \
	  -library build/ios/sim/lib$(1).a    -headers $(3) \
	  -output $(4)
	@echo "wrote $(4)"

IOS_XCFRAMEWORKS += $(4)/Info.plist
endef

ifneq ($(IOS_LAYOUT_HASH),)
IOS_SG_ARGS         = $(shell grep -v '^[[:space:]]*\#' $(IOS_SG_SPEC))
IOS_LAYOUT_TRIPLE  := arm64-apple-ios$(IOS_MIN_VERSION)
IOS_LAYOUT_TRIPLES := $(IOS_LAYOUT_TRIPLE) arm64-apple-ios$(IOS_MIN_VERSION)-simulator \
                      x86_64-apple-ios$(IOS_MIN_VERSION)-simulator

# REGENERATED ON EVERY MAKE THAT NEEDS IT (FORCE), written only when it
# differs. It costs a few parses, and a hash that could be from an older
# header than the library is the one failure the hash exists to catch.
#
# ONE HASH FOR THREE SLICES, checked rather than assumed: the readers are
# generated once, for the device triple, and are right on a simulator only
# because the three targets lay the structs out alike. A struct that stopped
# doing so would read right on two architectures and wrong on the third.
$(IOS_LAYOUT_HASH): $(STRUCTGEN) $(IOS_SG_SPEC) FORCE
	@mkdir -p $(@D)
	@for t in $(IOS_LAYOUT_TRIPLES); do \
	  $(STRUCTGEN) --cwd . $(IOS_SG_ARGS) --build 'ios=$(strip $(IOS_CFLAGS))' --target $$t --print-hash || exit 1; \
	done | sort -u > $@.new
	@[ "$$(grep -c . $@.new)" = 1 ] || { echo "layout ios: the iOS slices do not share one layout:"; cat $@.new; exit 1; }
	@{ cmp -s $@.new $@ || { echo "layout ios: $$(cat $@ 2>/dev/null || echo none) -> $$(cat $@.new)"; mv $@.new $@; }; }; rm -f $@.new
	@$(STRUCTGEN) --cwd . $(IOS_SG_ARGS) --build 'ios=$(strip $(IOS_CFLAGS))' --target $(IOS_LAYOUT_TRIPLE) \
	  --swift $@.swift && $(call gen_sync,$@.swift,$(IOS_KERNEL_SWIFT))

# THE SWIFT SIDE, ON THIS MAC, as a recipe: the readers generated for this
# machine's triple, the kernel ($(IOS_SRC)) compiled for it and stamped with
# that layout's hash, and a Swift program that drives the bridge through them.
# From clean every time - it is a smoke run, and a pair that cannot be stale is
# worth more here than a second saved. Needs swiftc and libclang.
#   $(1)  the bridge header Swift imports
#   $(2)  the readers' module name (the product's <Name>Kernel)
#   $(3)  the directory of the generated string tables
#   $(4)  the Swift program
#   $(5)  optional: its arguments
IOS_SWIFT_TRIPLE = $(shell swiftc -print-target-info 2>/dev/null | sed -n 's/.*"unversionedTriple": *"\([^"]*\)".*/\1/p' | head -1)
define IOS_SWIFT_SMOKE
@rm -rf build/swift && mkdir -p build/swift/obj
@$(STRUCTGEN) --cwd . $(IOS_SG_ARGS) --build 'host=$(strip $(IOS_CFLAGS))' --target $(IOS_SWIFT_TRIPLE) \
  --swift build/swift/$(2).swift --print-hash > build/swift/hash
@for s in $(IOS_SRC); do \
  $(CC) $(IOS_CFLAGS) -DSG_LAYOUT_HASH=$$(cat build/swift/hash) -c $$s -o build/swift/obj/$$(basename $$s .c).o || exit 1; \
done
swiftc -parse-as-library -import-objc-header $(1) build/swift/$(2).swift \
  $(3)/*.swift $(4) build/swift/obj/*.o -o build/swift/smoke
./build/swift/smoke $(5)
endef
endif
