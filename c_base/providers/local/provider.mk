# providers/local/provider.mk - the local provider's part of the build
# (docs/PLATFORM.md 8.5): its main and its artifact. Local deploys no
# infrastructure (the store is a SQLite file `cbase serve` opens).
#
#   make package-local    dist/<app>/local/: the cbase binary and the manifest
PROVIDERS += local
MAIN_local := providers/local/src/main_serve.c
LOCAL_DIST := dist/$(APP)/local

.PHONY: package-local
package-local: build/cbase build/cbase-describe
	@mkdir -p $(LOCAL_DIST)
	cp build/cbase $(LOCAL_DIST)/cbase
	{ echo provider=local; echo artifact=cbase; \
	  echo sha256=$$( (sha256sum $(LOCAL_DIST)/cbase 2>/dev/null || shasum -a 256 $(LOCAL_DIST)/cbase) | awk '{print $$1}'); \
	  echo entry=cbase; \
	  echo commit=$$(git rev-parse HEAD)$$(git diff --quiet HEAD -- .. 2>/dev/null || echo -dirty); \
	  build/cbase-describe --settings; } > $(LOCAL_DIST)/manifest.new
	mv $(LOCAL_DIST)/manifest.new $(LOCAL_DIST)/manifest
	@cat $(LOCAL_DIST)/manifest
