// An action wire's card bytes are cards, or the move is a malformed wire.
//
// awire_decode (c/src/awire.c) used to clamp a byte past the last card onto card
// 51, the ace of diamonds: a hostile [attack, 1, 0xFF] was APPLIED as that ace
// for a seat that held it, and the hidden card 0xFE reached the rules as a move
// and came back as a rules rejection. Every host decodes through that one
// function (the C Table's `action`, the iOS bridge, the web's move guard), so
// this drives it where a server does: table_act on a dealt table, whose first
// attacker holds the ace of diamonds.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fixture, fixtureTable, PLAYING } from './helpers/table_fixture.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';

function table() {
    const { state, roster } = fixture()
        .seats([{ id: 'ann', name: 'Ann' }, { id: 'bob', name: 'Bob' }])
        .status(PLAYING).attacker(0).defender(1)
        .hand(0, 'Ad 6s').hand(1, '7h 8h').trump('Kc').deck('9c')
        .build();
    const t = fixtureTable();
    assert.equal(t.load(state, roster), L.TABLE_OK, 'the dealt table loads');
    return t;
}

test('a card byte past the last card is a malformed wire, never a card', () => {
    for (const [what, wire] of [
        ['0xFF (no card)', [0, 1, 0xff]],
        ['0xFE (the hidden card)', [0, 1, 0xfe]],
        ['200', [0, 1, 200]],
        ['52', [0, 1, 52]],
        ["a cover's attack byte", [1, 1, 51, 0xff]],
    ] as [string, number[]][]) {
        assert.equal(table().act('ann', Uint8Array.from(wire), null, 0), L.TABLE_E_WIRE, `${what}: refused as a wire`);
    }
    assert.equal(table().act('ann', Uint8Array.from([0, 1, 51]), null, 0), L.TABLE_APPLIED, 'card 51 itself is the ace of diamonds, and plays');
});
