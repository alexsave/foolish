/* =============================================================================
 * The website's role marks are the iMessage ones, point for point
 * =============================================================================
 * The three marks a seat wears - sword (attacking), shield (defending), check
 * (said good) - were settled on the iMessage board over the owner's rounds 5, 7,
 * 12 and 20, and ios/FoolishKit/Boards/FRoleGlyphs.swift is where they live. The
 * website draws the same three (src/components/RoleMark.tsx), and "the same" has
 * to be a check rather than a promise: a path point nudged on one side and not
 * the other is exactly the drift nobody notices until the two boards are put
 * beside each other.
 *
 * So this reads BOTH files and compares them. It hard-codes no geometry of its
 * own: every number below is read out of the Swift or out of the TSX, so there is
 * no third copy to fall behind, and a change on either side that is not made on
 * the other fails here by name.
 *
 * WHAT IT DOES NOT HOLD. Only what crosses: the 24x24 path points, the ink, the
 * outline weights and the drawn sizes. How each host gets those on screen is its
 * own (SwiftUI Canvas there, SVG here), and the hosts differ in one detail this
 * normalizes: SwiftUI's addQuadCurve names its endpoint first and its control
 * second, SVG's Q the other way round.
 * ========================================================================== */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const SWIFT = readFileSync(new URL('../ios/FoolishKit/Boards/FRoleGlyphs.swift', import.meta.url), 'utf8');
const TSX = readFileSync(new URL('../src/components/RoleMark.tsx', import.meta.url), 'utf8');

type Pt = readonly [number, number];

/** The text between two markers, or a failure naming the marker that is gone. */
function between(source: string, what: string, from: string, to: string): string {
    const i = source.indexOf(from);
    assert.ok(i >= 0, `${what}: "${from}" is not in the source any more`);
    const j = source.indexOf(to, i + from.length);
    assert.ok(j >= 0, `${what}: "${to}" is not in the source any more`);
    return source.slice(i + from.length, j);
}

/** Every `P(x, y)` in a Swift path, in order. */
const swiftPoints = (segment: string): Pt[] =>
    [...segment.matchAll(/P\(\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*\)/g)].map((m) => [Number(m[1]), Number(m[2])] as const);

/** Every coordinate pair in an SVG path string, in order. */
const svgPoints = (path: string): Pt[] =>
    [...path.matchAll(/(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)/g)].map((m) => [Number(m[1]), Number(m[2])] as const);

/** A named `const NAME = '...'` (or a concatenation of them) out of the TSX. */
function tsxString(name: string): string {
    const decl = between(TSX, name, `const ${name} =`, ';');
    return [...decl.matchAll(/'([^']*)'/g)].map((m) => m[1]).join('');
}

const num = (source: string, what: string, re: RegExp): number => {
    const m = source.match(re);
    assert.ok(m, `${what}: ${re} found nothing`);
    return Number(m[1]);
};

test('the sword is the same outline, rotated the same way, with the same pommel', () => {
    const swift = swiftPoints(between(SWIFT, 'FSword', 'var sword = Path()', 'ctx.fill(sword'));
    const web = svgPoints(tsxString('SWORD_PATH'));
    assert.deepEqual(web, swift, 'FSword\'s blade, crossguard and grip');

    // Rotated to point it up-and-to-the-right, about the grid's own centre.
    assert.match(SWIFT, /\.rotationEffect\(\.degrees\(45\)\)/, 'FSword turns 45 degrees');
    assert.match(TSX, /transform="rotate\(45 12 12\)"/, 'the web sword turns 45 degrees about (12,12)');

    // The pommel: a knob of radius r centred on the base of the grip. Swift draws
    // it as a rect inset by r from its centre; SVG names the centre outright.
    const r = num(SWIFT, 'FSword pommel radius', /let r = ([\d.]+) \* s/);
    const cy = num(SWIFT, 'FSword pommel centre', /y: ([\d.]+) \* s - r/);
    const cx = num(SWIFT, 'FSword pommel centre', /x: (\d+) \* s - r/);
    const circle = TSX.match(/<circle cx="([\d.]+)" cy="([\d.]+)" r="([\d.]+)"/);
    assert.ok(circle, 'the web sword draws a pommel');
    assert.deepEqual([Number(circle[1]), Number(circle[2]), Number(circle[3])], [cx, cy, r], 'the pommel');
});

test('the shield is the same four curves', () => {
    // SwiftUI: addQuadCurve(to: P(x,y), control: P(cx,cy)). SVG: Q cx cy, x y.
    const segment = between(SWIFT, 'FShield', 'var shield = Path()', 'ctx.fill(shield');
    const move = swiftPoints(between(segment, 'FShield', 'shield.move(to:', ')\n'))[0];
    const curves = [...segment.matchAll(/addQuadCurve\(to: P\(([\d.]+),\s*([\d.]+)\),\s*control: P\(([\d.]+),\s*([\d.]+)\)\)/g)]
        .map((m) => ({ to: [Number(m[1]), Number(m[2])] as Pt, control: [Number(m[3]), Number(m[4])] as Pt }));
    assert.equal(curves.length, 4, 'FShield is four quad curves');

    const swift: Pt[] = [move, ...curves.flatMap((c) => [c.control, c.to])];
    assert.deepEqual(svgPoints(tsxString('SHIELD_PATH')), swift, 'the heater shield, peak to shoulders to point');
});

test('the check is the same three points and the same two widths', () => {
    const swift = swiftPoints(between(SWIFT, 'FCheck', 'var check = Path()', 'ctx.stroke(check'));
    assert.deepEqual(svgPoints(tsxString('CHECK_PATH')), swift, 'FCheck\'s two strokes');

    // A check is an open path, so "white fill + black outline" is two passes of
    // it: a fat line, then a thinner body. The widths differ by twice the family's
    // outline weight so the rim is even on both sides.
    const body = num(SWIFT, 'FCheck body width', /lineWidth: \(([\d.]+) \+ 2 \* FRoleInk\.stroke\)/);
    assert.equal(num(TSX, 'the web check body width', /const CHECK_BODY = ([\d.]+);/), body);
    assert.match(TSX, /strokeWidth=\{CHECK_BODY \+ 2 \* RoleInk\.stroke\}/, 'the rim is the body plus two outlines');
});

test('the ink is the same ink', () => {
    for (const name of ['line', 'good', 'lead'] as const) {
        const m = SWIFT.match(new RegExp(`static let ${name} = Color\\(hex: 0x([0-9A-Fa-f]{6})\\)`));
        assert.ok(m, `FRoleInk.${name} is still declared`);
        const web = TSX.match(new RegExp(`${name}: '#([0-9A-Fa-f]{6})'`));
        assert.ok(web, `RoleInk.${name} is still declared`);
        assert.equal(web[1].toUpperCase(), m[1].toUpperCase(), `FRoleInk.${name}`);
    }
    // The body the sword and the shield wear is plain white on both sides.
    assert.match(SWIFT, /static let fill = Color\.white/, 'FRoleInk.fill is white');
    assert.match(TSX, /fill: '#FFFFFF'/, 'RoleInk.fill is white');

    assert.equal(num(TSX, 'RoleInk.stroke', /stroke: ([\d.]+),/),
        num(SWIFT, 'FRoleInk.stroke', /static let stroke: CGFloat = ([\d.]+)/), 'the outline weight, in grid units');
});

test('the marks are drawn at the same sizes', () => {
    for (const name of ['check', 'shield', 'sword'] as const) {
        const swift = num(SWIFT, `FRoleMark.${name}`, new RegExp(`static let ${name}: CGFloat = ([\\d.]+)`));
        assert.equal(num(TSX, `RoleMarkSize.${name}`, new RegExp(`${name}: ([\\d.]+),`)), swift, `FRoleMark.${name}`);
    }
    // A role row is as tall as the largest glyph in it, or it clips the sword's
    // corners. Swift says that by writing the sword's own size down; the web has
    // to repeat the number, so this is what holds the two together.
    assert.match(SWIFT, /static let rowHeight: CGFloat = sword/, 'FRoleMark.rowHeight is the sword\'s size');
    assert.equal(num(TSX, 'RoleMarkSize.rowHeight', /rowHeight: ([\d.]+),/),
        num(SWIFT, 'FRoleMark.sword', /static let sword: CGFloat = ([\d.]+)/), 'FRoleMark.rowHeight');
});
