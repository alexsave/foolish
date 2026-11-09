// The [game_id] page the static export writes once (out/_shell.html). Behind
// CloudFront the app shell is served, with a 200, for every path that is not a
// file and not a known page (cbase's router: CBASE_SHELL in foolish/cbase.conf),
// so the router's param is this placeholder and the real path is in
// location.pathname (useUrlGameId). `next dev` renders the real param and never
// sees this value. Its own module so the server page can import it without the hook.
export const GAME_SHELL_PARAM = '_shell';

/**
 * The game route's segment in an address the shell was served for: the one
 * path segment, decoded, trailing slashes ignored ("/ab12cd/" is "ab12cd").
 * null when the address is not a game path - the root, two or more segments,
 * or a segment that does not decode - which the shell answers with the app's
 * own not-found view (the edge no longer has a 404 page for it).
 */
export function shellSegment(pathname: string): string | null {
    const path = pathname.replace(/\/+$/, '');
    const parts = path.split('/');
    if (parts.length !== 2 || parts[0] !== '' || parts[1] === '') return null;
    try {
        return decodeURIComponent(parts[1]);
    } catch {
        return null;
    }
}
