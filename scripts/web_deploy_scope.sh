#!/bin/bash
# Reads changed paths on stdin, writes the ones that can reach the served web
# bundle to stdout. Empty output means a preview deployment would prove nothing
# and should not spend one of Vercel's 100 deployments per day.
#
# It lives in its own file, rather than inline in .github/workflows/web.yml, so
# that e2e/validation/web_deploy_scope_validation.test.ts can feed it real paths
# and check the answers. A path filter nobody can test is a path filter that
# silently stops deploying the site.
#
# THE LIST IS AN INVERSE LIST, and that direction is the safety property:
# a path is skippable only when it PROVABLY cannot reach the bundle, and
# anything unrecognised deploys. Adding an entry here can take the site stale;
# forgetting one only costs a deployment.
#
# `c/**` IS NOT SKIPPABLE AND MUST NEVER BE ADDED. The web imports TypeScript
# modules generated from those headers (tools/structgen), and it ships
# sdk/ts/wasm/bots.wasm.gz built from that source. A C change absolutely can
# change what a visitor downloads.
#
# `server/impls/` is skippable only because NOTHING the web bundle imports lives
# there today. tsconfig.json does alias `@shared/*` into
# server/impls/supabase/functions/_shared/, so that could stop being true with a
# single import and nothing here would notice - the site would just quietly stop
# redeploying. e2e/validation/web_deploy_scope_validation.test.ts walks the
# web's REAL import graph and fails if anything reachable from it lives under a
# skipped prefix, so the assumption is enforced rather than remembered.
set -euo pipefail

# One extended regular expression per line, matched against the path.
SKIP='^docs/
(^|/)[^/]*\.md$
^ios/
^server/impls/
^experiments/
^rustpoc/
^foolyard/
^offlinefun/
^cnitro/
^e2e/
^tests/
^\.github/workflows/'

while IFS= read -r path; do
    [ -n "$path" ] || continue

    # web.yml is the one workflow that is NOT skippable: a change to this lane
    # has to prove itself by running, or it can only be tested by merging it.
    if [ "$path" = ".github/workflows/web.yml" ] || [ "$path" = "scripts/web_deploy_scope.sh" ]; then
        printf '%s\n' "$path"
        continue
    fi

    skip=0
    while IFS= read -r rx; do
        [ -n "$rx" ] || continue
        if printf '%s\n' "$path" | grep -qE "$rx"; then skip=1; break; fi
    done <<<"$SKIP"

    [ "$skip" = "1" ] || printf '%s\n' "$path"
done
