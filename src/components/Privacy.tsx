export const Privacy = () => {
    return (
        <div className="page page--centered page--with-padding" style={{ width: '100vw' }}>
            <div className="content content--max-width content--centered content--gap-lg z-content">
                <h1 className="title title--page">Privacy Policy</h1>

                <div className="about-content">
                    <p><em>Last updated: July 2026.</em></p>

                    <h2>The short version</h2>
                    <p>
                        Foolish is a Durak card game. Playing a game in iMessage sends
                        no data to us at all — the entire match lives inside the
                        message bubbles you and the other players exchange through
                        Apple&apos;s iMessage service, and is validated on each
                        player&apos;s own device. We never see it, never store it, and
                        have no server involved in an iMessage game.
                    </p>
                    <p>
                        The Foolish app itself can optionally be used to play offline
                        against computer opponents (no account, no network, no data
                        collected at all) or, if you choose to create an account, to
                        play online against other people. The sections below cover
                        exactly what that optional account involves.
                    </p>

                    <h2>What we collect</h2>
                    <p>
                        <strong>If you never create an account</strong> (this covers
                        every iMessage game, and offline play against bots): we collect
                        nothing. There is no analytics SDK, no advertising SDK, and no
                        tracking of any kind in this app.
                    </p>
                    <p>
                        <strong>If you create an account</strong> to play online: we
                        store a username, an account identifier, and the games you
                        play (so your match history, win/loss record, and rating can be
                        shown to you). None of this is used for advertising, none of it
                        is sold, and none of it is shared with third parties except the
                        infrastructure providers who host it for us (our database and
                        hosting providers, acting solely on our instructions).
                    </p>
                    <p>
                        <strong>Camera access</strong> is used only if you choose to
                        scan a QR code to load a replay. The camera feed never leaves
                        your device and nothing from it is stored or transmitted.
                    </p>
                    <p>
                        <strong>Crash diagnostics</strong> are collected by Apple, not
                        us, only if you have opted in at the iOS level (Settings →
                        Privacy → Analytics &amp; Improvements). We do not receive
                        these unless you separately choose to share a crash report with
                        us directly.
                    </p>

                    <h2>What we don&apos;t do</h2>
                    <ul>
                        <li>No tracking across other apps or websites (no ATT prompt is
                            shown because we don&apos;t need one).</li>
                        <li>No advertising, no ad SDKs, no ad identifiers.</li>
                        <li>No selling or renting of any data, ever.</li>
                        <li>No third-party analytics SDKs.</li>
                    </ul>

                    <h2>Children&apos;s privacy</h2>
                    <p>
                        Foolish is a standard card game with no gambling, wagering, or
                        real-money mechanics, and is not directed at children under 13.
                        We do not knowingly collect personal information from children
                        under 13. If you believe a child has created an account,
                        contact us using the details below and we will delete it.
                    </p>

                    <h2>Deleting your account</h2>
                    <p>
                        If you created an account, you can delete it and all data
                        associated with it at any time, from inside the app
                        (Settings → Delete Account) or from{' '}
                        <a href="/delete-account">this page</a>. Deletion is immediate
                        and permanent.
                    </p>

                    <h2>Changes to this policy</h2>
                    <p>
                        If this policy changes, we will update the date at the top of
                        this page. Continued use of the app after a change means you
                        accept the updated policy.
                    </p>

                    <h2>Contact</h2>
                    <p>
                        Questions about this policy or your data:{' '}
                        <a href="mailto:privacy@foolish.cards">privacy@foolish.cards</a>.
                    </p>
                </div>
            </div>
        </div>
    );
};
