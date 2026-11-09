// The one [code] page the static export writes (out/_shell.html). Behind
// CloudFront the shell is served, with a 200, for every path that is not a file
// and not a known page (cbase's router: CBASE_SHELL in uttt/cbase.conf), so the
// app reads the address itself (shellRoute): the code a visitor asked for is in
// location.pathname, not in the route param the page was prerendered with.
export const SHELL_CODE = '_shell';

/** Old addresses the site moved, answered by the app (the edge has no
 *  redirects): the 243 arena was at /234 for its first day. */
export const SHELL_REDIRECTS: Readonly<Record<string, string>> = { '/234': '/243' };

/** What the shell does with the address it was served for: go to another
 *  address (the search kept), play the one-segment code (decoded, trailing
 *  slashes ignored), or say there is no game here (a deeper path, or a segment
 *  that does not decode). */
export type ShellRoute = { redirect: string } | { code: string } | { notFound: true };

export function shellRoute(pathname: string, search: string): ShellRoute {
    const path = pathname.replace(/\/+$/, '');
    const to = SHELL_REDIRECTS[path];
    if (to) return { redirect: to + search };
    const parts = path.split('/');
    if (parts.length !== 2 || parts[0] !== '' || parts[1] === '') return { notFound: true };
    try {
        return { code: decodeURIComponent(parts[1]) };
    } catch {
        return { notFound: true };
    }
}
