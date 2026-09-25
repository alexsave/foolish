import type { Metadata } from 'next';
import styles from './about.module.css';

// uttt.live/about: the Marketing URL on the App Store record. The same shape as
// foolish's About (src/components/About.tsx): a title and three paragraphs, on
// this game's own paper. Static text, so it reads with JavaScript off.
export const dynamic = 'force-static';

export const metadata: Metadata = {
    title: 'About - Ultimate Tic-Tac-Toe',
    description: 'Ultimate Tic-Tac-Toe for iMessage: nine little boards, one big one, played one message at a time.',
};

export default function About() {
    return (
        <main className={styles.page}>
            <h1 className={styles.title}>About Ultimate Tic-Tac-Toe</h1>
            <div className={styles.content}>
                <p>
                    Ultimate Tic-Tac-Toe takes the game everyone already knows and plays it on nine boards at once,
                    where every move also decides where your opponent has to play next. Nobody seems to know who
                    invented it; it spread as a folk game on the internet.{' '}
                    <a href="https://en.wikipedia.org/wiki/Ultimate_tic-tac-toe">Wikipedia</a> has more about it.
                </p>
                <p>
                    It is a game that suits texting: two players, one move at a time, nothing hidden and no rush.
                    So it lives inside Messages. Every move is a message, and the newest one always carries the
                    whole game. There is nothing to sign up for and no server in the middle. When a game ends,
                    Copy code turns it into a link anyone can watch here on uttt.live.
                </p>
                <p>
                    It is built the same way as Foolish, my Durak game: one small C kernel holds the rules, the
                    drawing and the animation, and both the iPhone app and this website run it. The board is drawn
                    stroke by stroke, with a pen that wobbles like a hand, on a paper napkin, and the strokes are
                    the same on every screen.
                </p>
            </div>
        </main>
    );
}
