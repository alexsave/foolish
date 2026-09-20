// The Apple App Site Association file has to name the REAL app, not a placeholder.
//
// `public/.well-known/apple-app-site-association` is served verbatim by Next.js
// and is how iOS decides whether a foolish.cards link opens in the app or in
// Safari. It shipped to production reading:
//
//     "appID": "TEAMID.cards.foolish.app"
//
// `TEAMID` is not a Team ID. It is the word from Apple's documentation example,
// left in place and deployed, so for as long as it was live every universal link
// from the shipped App Store build fell through to the browser.
//
// This is the quiet kind of defect, which is why it survived: nothing fails. The
// JSON is valid, the file serves 200, the entitlement is present, the app builds
// and ships. The only symptom is a link opening the wrong thing, on a device, in
// someone else's hands - and the one person who would notice already has the app
// installed and taps links from the app.
//
// So the two halves are derived from ios/project.yml rather than restated: the
// prefix must be the DEVELOPMENT_TEAM the project actually signs with, and the
// suffix must be a bundle id that project actually declares. Change either in
// project.yml without changing this file and the mismatch is a red test instead
// of a dead link.
//
// Pure test - no Postgres, no network, no compiler.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const REPO = resolve(import.meta.dirname, '../..');
const AASA = join(REPO, 'public/.well-known/apple-app-site-association');
const PROJECT = join(REPO, 'ios/project.yml');
const ENTITLEMENTS = join(REPO, 'ios/FoolishApp/Foolish.entitlements');

/** The Team ID the Xcode project signs with. */
function developmentTeam(): string {
    const m = readFileSync(PROJECT, 'utf8').match(/^\s*DEVELOPMENT_TEAM:\s*([A-Z0-9]+)\s*$/m);
    assert.ok(m, 'could not find DEVELOPMENT_TEAM in ios/project.yml - did its shape change?');
    return m![1];
}

/** Every bundle id the Xcode project declares. */
function bundleIds(): string[] {
    const out = [...readFileSync(PROJECT, 'utf8').matchAll(/^\s*PRODUCT_BUNDLE_IDENTIFIER:\s*(\S+)\s*$/gm)]
        .map((m) => m[1]);
    assert.ok(out.length > 3, `only ${out.length} bundle ids parsed out of ios/project.yml - the parse is probably wrong`);
    return out;
}

type Aasa = { applinks?: { details?: { appID?: string; appIDs?: string[] }[] } };

function aasa(): Aasa {
    const raw = readFileSync(AASA, 'utf8');
    // Apple fetches this as application/json and does not tolerate comments or a
    // BOM, so a parse failure here is the same failure a device would hit.
    return JSON.parse(raw) as Aasa;
}

/** Every appID the file claims, however it spells them. */
function appIds(): string[] {
    const details = aasa().applinks?.details ?? [];
    assert.ok(details.length > 0, 'the AASA file declares no applinks details - no link would ever open the app');
    return details.flatMap((d) => [...(d.appID ? [d.appID] : []), ...(d.appIDs ?? [])]);
}

test('every appID is prefixed with the Team ID the iOS project actually signs with', () => {
    const team = developmentTeam();
    const wrong = appIds().filter((id) => id.split('.')[0] !== team);
    assert.deepEqual(wrong, [], [
        `These appIDs in public/.well-known/apple-app-site-association do not start with`,
        `the DEVELOPMENT_TEAM in ios/project.yml (${team}), so iOS will reject the`,
        'association and every universal link will open in Safari instead of the app:',
        ...wrong.map((id) => `  ${id}`),
    ].join('\n'));
});

test('every appID names a bundle the iOS project actually builds', () => {
    const known = new Set(bundleIds());
    const wrong = appIds().filter((id) => !known.has(id.split('.').slice(1).join('.')));
    assert.deepEqual(wrong, [], [
        'These appIDs name a bundle identifier that ios/project.yml does not declare,',
        'so they associate the domain with an app that does not exist:',
        ...wrong.map((id) => `  ${id}`),
        '',
        `Known bundle ids: ${[...known].join(', ')}`,
    ].join('\n'));
});

test('no documentation placeholder survives into the served file', () => {
    // The literal that shipped, plus the other spellings Apple's own docs and
    // the common tutorials use. A placeholder is valid JSON and serves 200, so
    // nothing else in this repo would ever notice one.
    const raw = readFileSync(AASA, 'utf8');
    const placeholders = ['TEAMID', 'TEAM_ID', 'ABCDE12345', 'YOUR_TEAM_ID', 'XXXXXXXXXX', 'PREFIX'];
    const found = placeholders.filter((p) => raw.includes(p));
    assert.deepEqual(found, [], [
        'public/.well-known/apple-app-site-association still contains a placeholder:',
        ...found.map((p) => `  ${p}`),
        '',
        `The real Team ID is the DEVELOPMENT_TEAM in ios/project.yml (${developmentTeam()}).`,
    ].join('\n'));
});

test('the app asks for the domain the file is published under', () => {
    // An AASA file nobody claims is inert, and an entitlement pointing at a
    // domain with no file is equally inert. They only work as a pair, and
    // nothing else checks that the pair is still a pair.
    const ent = readFileSync(ENTITLEMENTS, 'utf8');
    const domains = [...ent.matchAll(/applinks:([^<\s]+)/g)].map((m) => m[1]);
    assert.ok(domains.length > 0,
        'ios/FoolishApp/Foolish.entitlements declares no `applinks:` domain, so the app '
        + 'claims nothing and public/.well-known/apple-app-site-association is dead weight');
    // The file lives in this repo, which is the foolish.cards site. If the app
    // ever points somewhere else, this file stops being the one iOS reads.
    const off = domains.filter((d) => !d.endsWith('foolish.cards'));
    assert.deepEqual(off, [], [
        'The app claims these domains, which this repo does not publish an AASA file for:',
        ...off.map((d) => `  ${d}`),
    ].join('\n'));
});
