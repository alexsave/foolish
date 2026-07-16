// Server segment for /m/<payload> — it exists for generateMetadata.
//
// The page itself is a client component (it runs the kernel in the browser), and
// a client component cannot export metadata. But the unfurl has to be SERVER
// rendered: Messages, Slack, WhatsApp and the rest fetch the URL and read the
// <head>; none of them run our JavaScript. Without this, a shared link unfurls
// as a bare domain — and the unfurl is the whole free marketing surface the
// protocol gets (§13).
//
// The payload IS the game, so the server can just decode it and say something
// true. Server-side the big module loads from its static .gz (node:fs), the same
// bytes the browser gets from the base64 twin.
import type { Metadata } from 'next';

const TITLE = 'A Durak game in iMessage';
const BLURB = 'Hands stay hidden here. Watching is free.';

export async function generateMetadata(
    { params }: { params: Promise<{ payload: string }> },
): Promise<Metadata> {
    let title = TITLE;
    let description = BLURB;

    try {
        const { payload } = await params;
        const text = decodeURIComponent(payload || '').trim();
        // The leading char is the text-level format version (§4.3) — dispatch
        // before decoding any binary.
        if (text[0] === '1') {
            const { kernelMsgDecode, ensureBotsAsync } = await import('@shared/wasm/bots.ts');
            // Await the module's door even though this is the SERVER. The sync
            // read works here only if wasm_asset's `require` probe finds a real
            // require — and Next's server bundle is ESM, where it may not. The
            // async path is correct in both runtimes and is a no-op once loaded.
            await ensureBotsAsync();
            const { base32Decode } = await import('@shared/replay/codec.ts');
            const env = kernelMsgDecode(base32Decode(text.slice(1)));

            // Nicknames are the only identity a payload carries, and they are
            // self-reported (§4.1) — there is nothing else to put here, which is
            // also why this is safe to unfurl.
            const names = env.joins.map(j => j.name).filter(Boolean);
            const who = names.length >= 2 ? names.join(' vs ') : `${env.n_players} players`;
            title = env.phase === 3 ? `${who} — a finished Durak game` : `${who} — turn ${env.turn}`;
            description = `A live iMessage Durak game. ${BLURB}`;
        }
    } catch {
        // A damaged or hostile link still gets a real page and honest tags —
        // never a stack trace, and never a claim about a game we could not read.
    }

    return {
        title,
        description,
        // No og:image yet: the bubble snapshot is the extension's renderer (M2),
        // and the web has no equivalent endpoint. Text unfurls correctly without
        // one; an ImageResponse route is the follow-up.
        openGraph: { title, description, type: 'website', siteName: 'Foolish' },
        twitter: { card: 'summary', title, description },
        // A game link is not a search result — it is one specific game, and the
        // payload can be long-lived. Keep it out of the index.
        robots: { index: false, follow: true },
    };
}

export default function MessagePayloadLayout({ children }: { children: React.ReactNode }) {
    return <>{children}</>;
}
