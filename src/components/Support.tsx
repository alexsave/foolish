export const Support = () => {
    return (
        <div className="page page--centered page--with-padding" style={{ width: '100vw' }}>
            <div className="content content--full-width content--centered content--gap-lg z-content">
                <h1 className="title title--page">Support</h1>

                <div className="about-content">
                    <p>
                        Foolish is a Durak (дурак) card game, playable inside iMessage
                        and, offline, against computer opponents.
                    </p>

                    <h2>How to play</h2>
                    <p>
                        In Messages, tap the App Store icon in the compose bar, find
                        Foolish, and tap New game. See{' '}
                        <a href="/about">About</a> for the rules.
                    </p>

                    <h2>Something not working?</h2>
                    <p>
                        Email us at{' '}
                        <a href="mailto:alexvsaveliev@gmail.com">alexvsaveliev@gmail.com</a>{' '}
                        with a description of what happened and, if you can, which
                        device and iOS version you&apos;re on. We read every message.
                    </p>

                    <h2>Delete your account</h2>
                    <p>
                        <a href="/delete-account">This page</a> deletes your account
                        and all associated data immediately. You can also do this from
                        Settings inside the app.
                    </p>

                    <h2>Privacy</h2>
                    <p>
                        See our <a href="/privacy">Privacy Policy</a> for what we do
                        and don&apos;t collect.
                    </p>
                </div>
            </div>
        </div>
    );
};
