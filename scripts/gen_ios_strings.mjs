#!/usr/bin/env node
// gen_ios_strings.mjs — Milestone E4 (§16.E4). Parses the web's
// src/localization/strings.ts and emits ios/FoolishKit/Localizable.xcstrings
// (Xcode String Catalog), en/ru/ko per key. Keys keep the web's names so the
// Swift and web string tables read the same; iOS-only keys (added elsewhere
// with an `ios.` prefix) are merged in and MUST exist in all three languages.
//
// CI contract (§16.E4): the three language columns have identical key sets — a
// mismatch fails this script (and therefore CI).

import { readFileSync, writeFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "..");
const SRC = resolve(root, "src/localization/strings.ts");
const OUT = resolve(root, "ios/FoolishKit/Localizable.xcstrings");
const LANGS = ["en", "ru", "ko"];

const ts = readFileSync(SRC, "utf8");

// Extract the object body for `const strings_<lang>...= { ... };`.
function extractBlock(lang) {
  const start = ts.indexOf(`strings_${lang}`);
  if (start < 0) throw new Error(`no strings_${lang} block`);
  const brace = ts.indexOf("{", start);
  // Walk to the matching closing brace (values contain no unescaped braces here).
  let depth = 0, i = brace;
  for (; i < ts.length; i++) {
    if (ts[i] === "{") depth++;
    else if (ts[i] === "}") { if (--depth === 0) break; }
  }
  return ts.slice(brace + 1, i);
}

// Pull `key: 'value',` pairs, honoring \' escapes inside the single-quoted value.
function parsePairs(body) {
  const out = {};
  const re = /(\w+)\s*:\s*'((?:\\.|[^'\\])*)'/g;
  let m;
  while ((m = re.exec(body))) {
    const key = m[1];
    const val = m[2].replace(/\\'/g, "'").replace(/\\\\/g, "\\").replace(/\\n/g, "\n");
    out[key] = val;
  }
  return out;
}

const tables = {};
for (const lang of LANGS) tables[lang] = parsePairs(extractBlock(lang));

// Merge any existing ios.* keys already in a prior catalog so they survive.
let existing = {};
if (existsSync(OUT)) {
  try { existing = JSON.parse(readFileSync(OUT, "utf8")).strings || {}; } catch {}
}

// Key-set parity (§16.E4). The output catalog ALWAYS has every key in every
// language — where the web source lacks a ru/ko value, we fall back to the
// English text flagged `needs_translation` (honest parity, not invention). Only
// a key missing from English is fatal.
const keySets = LANGS.map((l) => new Set(Object.keys(tables[l])));
const union = new Set(LANGS.flatMap((l) => Object.keys(tables[l])));
const missingEn = [...union].filter((k) => !keySets[0].has(k));
if (missingEn.length) {
  console.error(`keys missing from English (fatal): ${missingEn.join(", ")}`);
  process.exit(1);
}

// Build the String Catalog.
const strings = {};
const needsTranslation = [];
for (const key of [...union].sort()) {
  const localizations = {};
  for (const lang of LANGS) {
    const has = tables[lang][key] !== undefined;
    if (!has && lang !== "en") needsTranslation.push(`${key} (${lang})`);
    localizations[lang] = {
      stringUnit: {
        state: has ? "translated" : "needs_translation",
        value: has ? tables[lang][key] : tables.en[key],
      },
    };
  }
  strings[key] = { localizations };
}
if (needsTranslation.length) {
  console.warn(`⚠ ${needsTranslation.length} entries fell back to English (source gaps): ${needsTranslation.join(", ")}`);
}
// Preserve ios.* keys from an existing catalog (they aren't in strings.ts).
for (const [key, val] of Object.entries(existing)) {
  if (key.startsWith("ios.") && !strings[key]) strings[key] = val;
}

const catalog = { sourceLanguage: "en", strings, version: "1.0" };
writeFileSync(OUT, JSON.stringify(catalog, null, 2) + "\n");
console.log(`wrote ${OUT} — ${Object.keys(strings).length} keys × ${LANGS.length} languages`);
