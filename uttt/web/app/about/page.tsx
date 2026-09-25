import type { Metadata } from 'next';
import { rules } from '../../lib/kernel-build';
import styles from './about.module.css';

// uttt.live/about: what the game is, how it is played in Messages, and how to
// reach us - the Marketing and Support URLs on the App Store record. It is
// PRERENDERED: the rules come from the kernel's own table at build time
// (lib/kernel-build.ts), so the page reads the same with JavaScript off, and
// says exactly what the app's rules sheet says.
export const dynamic = 'force-static';

export const metadata: Metadata = {
    title: 'About - Ultimate Tic-Tac-Toe',
    description: 'Ultimate Tic-Tac-Toe for iMessage: nine little boards, one big one, played with a friend in a Messages conversation.',
};

const CONTACT = 'alexvsaveliev@gmail.com';

export default function About() {
    const r = rules();
    return (
        <div className={styles.sheet}>
            <main className={styles.col}>
                <header>
                    <span className={styles.label}>About</span>
                    <h1 className={styles.title}>Ultimate Tic-Tac-Toe</h1>
                    <p className={styles.lede}>
                        Tic-tac-toe with a twist, played with a friend right in Messages. Every move is a
                        message, drawn in pen on a paper napkin.
                    </p>
                </header>

                <section className={styles.section} aria-labelledby="rules">
                    <h2 id="rules" className={styles.h2}>{r.title}</h2>
                    <ul className={styles.list}>
                        {r.lines.map((line) => <li key={line}>{line}</li>)}
                    </ul>
                    <p className={styles.p}>
                        <span className={styles.x}>X</span> moves first. The board you have to play in is
                        marked with the <span className={styles.hl}>yellow highlighter</span>.
                    </p>
                </section>

                <section className={styles.section} aria-labelledby="play">
                    <h2 id="play" className={styles.h2}>Playing in Messages</h2>
                    <ol className={styles.list}>
                        <li>Open a conversation in Messages.</li>
                        <li>Tap the apps button next to the text field and choose <strong>Ultimate</strong>.
                            An invitation with an empty board goes into the text field. Send it.</li>
                        <li>Your friend taps the invitation and plays first, as <span className={styles.x}>X</span>.
                            You are <span className={styles.o}>O</span>.</li>
                        <li>Take turns. Each move is a new message, and the newest one always carries the
                            whole game.</li>
                        <li>Before you send, you can change your mind: tap another square in the highlighted
                            board.</li>
                    </ol>
                    <p className={styles.p}>
                        There is no Home Screen icon. The game lives in Messages, in the apps row next to the
                        text field. In a group conversation, anyone else who taps the game can watch.
                    </p>
                </section>

                <section className={styles.section} aria-labelledby="replays">
                    <h2 id="replays" className={styles.h2}>Replays</h2>
                    <p className={styles.p}>
                        When a game is over, tap <strong>Copy code</strong> to put a link to it on your
                        clipboard. The link opens the whole game here on uttt.live, move by move, for anyone,
                        with no app needed.
                    </p>
                </section>

                <section className={styles.section} aria-labelledby="support">
                    <h2 id="support" className={styles.h2}>Support</h2>
                    <p className={styles.p}>
                        Something not working, or a question? Email{' '}
                        <a href={`mailto:${CONTACT}`}>{CONTACT}</a>. Tell us what happened and, if you can,
                        which iPhone and iOS version you are on.
                    </p>
                    <p className={styles.p}>
                        No accounts, no ads, no tracking, no purchases. The iMessage app collects nothing; see
                        its <a href="/privacy-msg">privacy policy</a>.
                    </p>
                </section>

                <footer className={styles.foot}>
                    <a href="/">uttt.live</a>
                    <a href="/privacy-msg">iMessage app privacy</a>
                    <a href={`mailto:${CONTACT}`}>Contact</a>
                </footer>
            </main>
        </div>
    );
}
