// The rating a new player starts from is said in two places that cannot share a
// definition, and this holds them equal.
//
// The kernel rates a seat that has no rating yet from TABLE_RATING_START
// (c/src/table.h table_rate), and stamps that number on the game as the seat's
// "before". The database gives the same seat its first row from the rating
// columns' DEFAULT (c_base/sql/schema.sql: the store inserts the row and adds
// the game's change to it). If the two numbers differ, the finish screen says "1000 -> 1005"
// while the leaderboard says 905.
//
// Pure test - no Postgres, no network.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve, join } from 'node:path';
import * as L from '../../sdk/ts/gen/game_layout.bots.ts';

const SEED = readFileSync(join(resolve(import.meta.dirname, '../../..'), 'c_base/sql/schema.sql'), 'utf8');

test('the rating tables start a row where the kernel starts a seat', () => {
    for (const table of ['ratings', 'bots']) {
        const create = SEED.match(new RegExp(`CREATE TABLE IF NOT EXISTS ${table} \\(([\\s\\S]*?)\\n\\);`));
        assert.ok(create, `schema.sql creates ${table}`);
        const column = create[1].match(/^\s*elo_rating\s+INTEGER NOT NULL DEFAULT (\d+),/m);
        assert.ok(column, `${table}.elo_rating has a default this test can read`);
        assert.equal(Number(column[1]), L.TABLE_RATING_START, `${table}.elo_rating starts at the kernel's TABLE_RATING_START`);
    }
});
