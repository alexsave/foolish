import type { Metadata } from 'next';
import { Replay } from '../../components/Replay';

// uttt.live/<code>: the link the game's end screen copies (uttt_replay_url).
// The code says which format it is (uttt_code.h): "9" and base32 for a
// game's look and moves, base32 alone for a 1.0(8) link. The kernel reads
// both (uttt_replay_read); the page hands it the code as it came.
export const metadata: Metadata = { title: 'Ultimate Tic-Tac-Toe replay' };

export default async function Page({ params }: { params: Promise<{ code: string }> }) {
    const { code } = await params;
    return <Replay code={decodeURIComponent(code)} />;
}
