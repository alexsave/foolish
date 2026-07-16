// The unfurl is server-rendered, so prove generateMetadata on the server side —
// the same door Messages/Slack/WhatsApp come through (they run no JS).
import test from 'node:test';
import assert from 'node:assert/strict';
import { base32Encode } from '../supabase/functions/_shared/common/replay/codec.ts';

const FIXTURE = 'f7020002efcdab89674523010a0000030001000000000000000079d87206410d37d302c19dfb6cacbc8bebf879d242622082315709cc0f183788030004416e6e300104416e6e310204416e6e320a00012cb4fce6acbe29ba5d0adae18a66b4fdc7f6';
const bytes = Uint8Array.from(FIXTURE.match(/../g)!.map(b => parseInt(b, 16)));

test('a real payload unfurls with the game in it', async () => {
    const { generateMetadata } = await import('../src/app/m/[payload]/layout.tsx');
    const m = await generateMetadata({ params: Promise.resolve({ payload: '1' + base32Encode(bytes) }) });
    assert.match(String(m.title), /Ann0 vs Ann1 vs Ann2/);
    assert.match(String(m.title), /turn 10/);
    assert.match(String(m.description), /Hands stay hidden/);
    assert.equal(m.openGraph?.title, m.title);
    assert.equal((m.robots as any)?.index, false, 'one game, not a search result');
});

test('a damaged link still unfurls, and claims nothing about a game it cannot read', async () => {
    const { generateMetadata } = await import('../src/app/m/[payload]/layout.tsx');
    for (const bad of ['', '1', 'xyz', '1AAAAAAA', '9' + base32Encode(bytes)]) {
        const m = await generateMetadata({ params: Promise.resolve({ payload: bad }) });
        assert.equal(m.title, 'A Durak game in iMessage', `bad payload ${bad!.slice(0,8)} leaked a claim`);
        assert.ok(m.description, 'still has a description');
    }
});
