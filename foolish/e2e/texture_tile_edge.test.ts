// A seeded wood plank never shows the texture's tile edge (src/components/TexturedSurface.tsx).
//
// The wood is a generated picture, not a seamless tile, so a plank laid across
// the place where it repeats wears a hard seam. The lobby seeds each player's
// plank from the first two characters of the player's id, and every id whose
// first two characters were both letters (a UUID's a-f) put the plank across
// the edge: six bots in a lobby showed the same vertical crack through all six
// cards. This walks every two-character prefix a UUID can start with, at both
// texture scales, and checks the plank's window into the picture.
//
// Pure: no DOM, no Postgres.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { getTextureStyle, seedFromString } from '../src/components/TexturedSurface.tsx';

/** The largest plank the lobby lays a seeded texture on, rim included (a player card, a lobby button). */
const PLANK = { width: 260, height: 60 };
const HEX = '0123456789abcdef';

/** Where in the picture the plank's left (top) edge falls, for a CSS background-position `p` on a `tile`. */
const windowStart = (p: number, tile: number): number => ((tile - (p % tile)) % tile);

test('a seeded plank lies inside one copy of the wood, never across its edge', () => {
    for (const willRotate of [false, true]) {
        for (const a of HEX) {
            for (const b of HEX) {
                const id = `${a}${b}000000-0000-4000-8000-000000000000`;
                const style = getTextureStyle('blob:wood', false, seedFromString(id), willRotate);
                const [tw, th] = String(style.backgroundSize).split(' ').map((v) => parseFloat(v));
                const [px, py] = String(style.backgroundPosition).split(' ').map((v) => parseFloat(v));
                const x = windowStart(px, tw), y = windowStart(py, th);
                assert.ok(x + PLANK.width <= tw, `an id starting "${a}${b}" (rotating ${willRotate}) lays its plank across the wood's right edge: x ${x} of ${tw}`);
                assert.ok(y + PLANK.height <= th, `an id starting "${a}${b}" (rotating ${willRotate}) lays its plank across the wood's bottom edge: y ${y} of ${th}`);
            }
        }
    }
});
