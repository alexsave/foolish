# providers/aws/provider.mk - the aws provider's part of the build
# (docs/PLATFORM.md 8.5): its main, its artifact and its infrastructure.
#
#   make bootstrap        dist/bootstrap.zip: the static Linux arm64 Lambda binary,
#                         built in a pinned Alpine container (ops/build_bootstrap.sh)
#   make dist/<app>/aws/template.yaml
#                         the app's CloudFormation template: build/cbase-describe
#                         fills ops/app.yaml.in from the app's descriptor, twice,
#                         and the two must be the same bytes
#   make package-aws      dist/<app>/aws/: bootstrap.zip, template.yaml and the
#                         manifest (ops/package.sh runs it and checks the zip)
PROVIDERS += aws
MAIN_aws := providers/aws/src/main_lambda.c
AWS_SKELETON := providers/aws/ops/app.yaml.in
AWS_DIST := dist/$(APP)/aws
AWS_TEMPLATE := $(AWS_DIST)/template.yaml

.PHONY: bootstrap package-aws
bootstrap:
	providers/aws/ops/build_bootstrap.sh

$(AWS_TEMPLATE): build/cbase-describe $(AWS_SKELETON)
	@mkdir -p $(@D)
	build/cbase-describe --provider aws $(AWS_SKELETON) > $@.new
	@build/cbase-describe --provider aws $(AWS_SKELETON) | cmp -s - $@.new || { echo "cbase-describe: two runs wrote different templates"; rm -f $@.new; exit 1; }
	@mv $@.new $@

# The manifest: fixed name=value lines (docs/PLATFORM.md 8.5), the artifact's
# sha256 and the commit it was built from ("-dirty" when the tree was not
# clean), then the app's settings (describe.h cb_aws_settings).
package-aws: bootstrap $(AWS_TEMPLATE) build/cbase-describe
	cp dist/bootstrap.zip $(AWS_DIST)/bootstrap.zip
	{ echo provider=aws; echo artifact=bootstrap.zip; \
	  echo sha256=$$( (sha256sum $(AWS_DIST)/bootstrap.zip 2>/dev/null || shasum -a 256 $(AWS_DIST)/bootstrap.zip) | awk '{print $$1}'); \
	  echo entry=bootstrap; \
	  echo commit=$$(git rev-parse HEAD)$$(git diff --quiet HEAD -- .. 2>/dev/null || echo -dirty); \
	  build/cbase-describe --settings; } > $(AWS_DIST)/manifest.new
	mv $(AWS_DIST)/manifest.new $(AWS_DIST)/manifest
	@cat $(AWS_DIST)/manifest
