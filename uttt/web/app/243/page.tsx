import type { Metadata } from 'next';
import { Arena } from '../../components/Arena';

// uttt.live/243: two bots playing Ultimate Tic-Tac-Toe on the 243 x 243 board
// (the recursive game at depth 5), in the visitor's browser, as fast as it
// goes. A static route, so it wins over [code]: 243 is not a replay code.
export const metadata: Metadata = {
    title: 'Ultimate Tic-Tac-Toe, 243 x 243',
    description: 'Two bots play recursive Ultimate Tic-Tac-Toe on a 243 by 243 board, live in your browser.',
};

export default function Page() {
    return <Arena />;
}
