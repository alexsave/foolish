import type { Metadata } from 'next';
import { Replay } from '../../components/Replay';

// uttt.live/<code>: the link the game's end screen copies (uttt_replay_url).
// A catch-all, because the code is a path of its own: a format segment and
// the base32 ("2/ABC..."), or the base32 alone for a 1.0(8) link. The kernel
// reads both (uttt_replay_read); the page hands it the path as it came.
export const metadata: Metadata = { title: 'Ultimate Tic-Tac-Toe replay' };

export default async function Page({ params }: { params: Promise<{ code: string[] }> }) {
    const { code } = await params;
    return <Replay code={code.map(decodeURIComponent).join('/')} />;
}
