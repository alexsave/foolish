import styles from '../components/Replay.module.css';

// uttt.live with no code: nothing to replay, so say where a code comes from,
// and where the rest of the site is.
export default function Home() {
    return (
        <main className={styles.page}>
            <header className={styles.head}>
                <h1 className={styles.title}>Ultimate Tic-Tac-Toe</h1>
                <span className={styles.label}>Replays</span>
            </header>
            <p className={styles.line} style={{ textAlign: 'left' }}>
                Finish a game in Messages and tap Copy code. The link opens the whole game here, move by move.
            </p>
            <nav className={styles.links} aria-label="About this site">
                <a href="/about">About</a>
                <a href="/privacy">Privacy</a>
            </nav>
        </main>
    );
}
