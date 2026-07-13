#!/usr/bin/env node
// gen_ios_goldens.mjs — regenerates ios/Fixtures/goldens.json (§16.A3).
//
// The golden vectors are produced by the C bridge itself (cnitro/ios/
// ios_goldens.c), which links the SAME game.c/legal.c/view.c as the production
// wasm kernel — so native == wasm by construction, and these fixtures pin the
// bridge's JSON surface. The Swift EngineGoldenTests replay the same seeds
// through libfoolish.a and assert byte-equality (the keystone check).
//
// This wrapper exists because §16.A3 names `scripts/gen_ios_goldens.mjs` as the
// entry point. It shells out to `make ios-goldens`. A future revision can
// additionally diff the output against the e2e wasm oracle (e2e/harness.ts) for
// cross-toolchain verification once the wasm build is wired into CI here.

import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const cnitro = resolve(here, "..", "cnitro");

try {
  execFileSync("make", ["ios-goldens"], { cwd: cnitro, stdio: "inherit" });
  console.log("goldens.json regenerated (cnitro/ios/ios_goldens.c → ios/Fixtures/goldens.json).");
} catch (e) {
  console.error("failed to generate goldens:", e.message);
  process.exit(1);
}
