import type { Metadata } from 'next';
import { Replay } from '../../components/Replay';

// uttt.live/<code>: the link the game's end screen copies (uttt_replay_url).
export const metadata: Metadata = { title: 'Ultimate Tic-Tac-Toe replay' };

export default async function Page({ params }: { params: Promise<{ code: string }> }) {
    const { code } = await params;
    return <Replay code={decodeURIComponent(code)} />;
}
