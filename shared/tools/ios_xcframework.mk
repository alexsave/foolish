# The iOS xcframework recipe: one static library, built for the device and
# for both simulator architectures, wrapped with its headers. Included by each
# product's c/Makefile, which calls it from its own `ios-lib` recipe:
#
#     include ../../shared/tools/ios_xcframework.mk
#     ios-lib:
#     	$(call IOS_XCFRAMEWORK,name,$(IOS_SRC),$(IOS_CFLAGS),ios/include,17.0,../ios/vendor/Name.xcframework)
#
#   $(1) the library name: the slices are build/ios/<slice>/lib$(1).a
#   $(2) the C sources, compiled one by one into every slice
#   $(3) the compiler flags; a `$$(...)` in them reaches the shell unexpanded,
#        so a flag can read a stamp the recipe's prerequisites just wrote
#   $(4) the headers directory (the public header and its module.modulemap)
#   $(5) the minimum iOS version, the suffix of every target triple
#   $(6) the xcframework to write, replaced whole
#
# Everything lands under build/ios/ relative to the calling Makefile, and needs
# Xcode (xcrun, libtool, lipo, xcodebuild). The host-compiler twin of the same
# sources is each product's `ios-smoke`, which needs no Mac.

# One slice. $(1)=target triple  $(2)=sdk  $(3)=out dir  $(4)=library name
# $(5)=sources  $(6)=cflags
define IOS_XCF_SLICE
	@rm -rf $(3)/obj
	@mkdir -p $(3)/obj
	@for s in $(5); do \
	  o=$(3)/obj/$$(basename $$s .c).o; \
	  echo "  CC[$(2)] $$s"; \
	  xcrun -sdk $(2) clang -target $(1) $(6) -c $$s -o $$o || exit 1; \
	done
	libtool -static -o $(3)/lib$(4).a $(3)/obj/*.o
endef

define IOS_XCFRAMEWORK
	@echo "== device slice (arm64) =="
	$(call IOS_XCF_SLICE,arm64-apple-ios$(5),iphoneos,build/ios/device,$(1),$(2),$(3))
	@echo "== simulator slices (arm64 + x86_64) =="
	# BOTH, because `-destination 'generic/platform=iOS Simulator'` builds both
	# and a one-arch library fails the link with "ignoring file ... found
	# architecture arm64, required architecture x86_64" - which reads like a
	# missing symbol and is not.
	$(call IOS_XCF_SLICE,arm64-apple-ios$(5)-simulator,iphonesimulator,build/ios/sim-arm64,$(1),$(2),$(3))
	$(call IOS_XCF_SLICE,x86_64-apple-ios$(5)-simulator,iphonesimulator,build/ios/sim-x86_64,$(1),$(2),$(3))
	@mkdir -p build/ios/sim
	lipo -create build/ios/sim-arm64/lib$(1).a build/ios/sim-x86_64/lib$(1).a \
	     -output build/ios/sim/lib$(1).a
	@rm -rf $(6)
	@mkdir -p $(dir $(6))
	xcodebuild -create-xcframework \
	  -library build/ios/device/lib$(1).a -headers $(4) \
	  -library build/ios/sim/lib$(1).a    -headers $(4) \
	  -output $(6)
	@echo "wrote $(6)"
endef
