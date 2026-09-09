'use client';

// /m/<payload> — redirects to the homepage.
//
// It used to render the game: the payload IS the whole game, so the page decoded
// it and showed a spectator board with every hand masked (view.c's spectator
// view). That was real, and it is in git — but it was a SNAPSHOT. The link
// carries the game as it stood when that bubble was sent, and nothing updates it
// as the thread goes on, so anyone who opened it after the next move was looking
// at a stale board with no way to tell. A replay code that silently lies about
// being live is worse than no page.
//
// WHY CLIENT-SIDE, and not a server redirect or middleware. The unfurl in
// layout.tsx is server-rendered on purpose: Messages, Slack and WhatsApp fetch
// the URL and read the <head>, and none of them run JavaScript. A server redirect
// is followed by those crawlers too, so the shared link would unfurl as the bare
// homepage and the one free marketing surface the protocol has (§13) would be
// gone. Redirecting in the browser splits the two cleanly — a crawler still gets
// the game-specific card, a human gets the homepage.
//
// WHEN THE APP SHIPS this becomes the App Store URL instead of '/'. That is the
// whole change: one constant, and the blurb in layout.tsx that goes with it.
import { useEffect } from 'react';
import { useRouter } from 'next/navigation';
import Link from 'next/link';

const DESTINATION = '/';

export default function MessagePayloadPage() {
    const router = useRouter();

    useEffect(() => {
        router.replace(DESTINATION);
    }, [router]);

    // Shown for the frame before the redirect lands, and to anyone with
    // JavaScript off — for whom the link has to still go somewhere by hand.
    return (
        <main className="flex min-h-screen items-center justify-center p-8">
            <p className="text-sm opacity-70">
                Taking you to <Link href={DESTINATION} className="underline">foolish.cards</Link>…
            </p>
        </main>
    );
}
