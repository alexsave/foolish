import type { Metadata } from 'next';
import { ReplayAtPath } from '../../components/Replay';
import { SHELL_CODE } from '../../lib/shell';

// uttt.live/<code>: the link the game's end screen copies (uttt_replay_url).
// The code says which format it is (uttt_code.h): "9" and base32 for a
// game's look and moves, base32 alone for a 1.0(8) link. The kernel reads
// both (uttt_replay_read); the page hands it the code as it came.
export const metadata: Metadata = { title: 'Ultimate Tic-Tac-Toe replay' };

// The static export writes one shell for every code (lib/shell.ts); a server
// build still renders any other code on demand.
export function generateStaticParams() {
    return [{ code: SHELL_CODE }];
}

export default async function Page({ params }: { params: Promise<{ code: string }> }) {
    const { code } = await params;
    return <ReplayAtPath code={decodeURIComponent(code)} />;
}
