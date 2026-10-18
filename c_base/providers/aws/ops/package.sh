#!/bin/bash
# The aws provider's package (docs/PLATFORM.md 8.5): dist/<app>/aws/ with
# bootstrap.zip, template.yaml and the manifest, and c_base/build/cbase-lambda.zip
# (the same zip, the name CI's code deploy uploads) with its sha256.
#
#   c_base/providers/aws/ops/package.sh
#
# The zip is `make -C c_base bootstrap` (providers/aws/ops/build_bootstrap.sh: a
# static Linux arm64 `bootstrap` built in a pinned Alpine container, the same
# bytes every run); the template is the app's descriptor written into
# providers/aws/ops/app.yaml.in by build/cbase-describe; `make package-aws`
# (providers/aws/provider.mk) makes both and the manifest. This script runs it,
# checks the zip holds exactly one executable named `bootstrap` (what
# provided.al2023 runs), and checks the manifest names that zip.
#
# Needs Docker, and the native build's compiler (cbase-describe is built
# here). Run on this Mac or on the CI's ubuntu runner.
set -euo pipefail

here="$(cd "$(dirname "$0")/../../.." && pwd)"   # c_base/
die() { echo "package.sh: $*" >&2; exit 1; }
make -C "$here" package-aws
app="$(make -s -C "$here" print-app)"
dist="$here/dist/$app/aws"
zip="$dist/bootstrap.zip"

# The zip must hold `bootstrap` at its root and nothing else, executable.
listing="$(unzip -Z1 "$zip")"
[ "$listing" = "bootstrap" ] || die "$zip holds [$listing], expected exactly [bootstrap]"
mode="$(unzip -Z "$zip" bootstrap | awk '{print $1}')"
case "$mode" in
  -rwx*) ;;
  *) die "bootstrap in the zip is $mode, not executable" ;;
esac

if command -v sha256sum >/dev/null 2>&1; then
  sum="$(sha256sum "$zip" | awk '{print $1}')"
else
  sum="$(shasum -a 256 "$zip" | awk '{print $1}')"
fi
grep -qx "sha256=$sum" "$dist/manifest" || die "$dist/manifest does not name $zip's sha256 $sum"
out="$here/build/cbase-lambda.zip"
mkdir -p "$here/build"
cp "$zip" "$out"
echo "$sum  cbase-lambda.zip" > "$out.sha256"
size="$(wc -c < "$out" | tr -d ' ')"

cat <<MSG

The package, $dist:
  bootstrap.zip    $size bytes, sha256 $sum (also $out)
  template.yaml    the stack (providers/aws/ops/deploy.sh $dist deploys it)
  manifest         $(tr '\n' ' ' < "$dist/manifest")
MSG
