// The [game_id] page the static export writes once (out/_shell.html). On S3
// behind CloudFront (c_base/ops/web.yaml) the viewer-request Function answers
// every unknown one-segment path with that file, so the router's param is this
// placeholder and the real segment is in location.pathname (useUrlGameId). A
// server build (Vercel, until DNS moves) renders the real param and never sees
// this value. Its own module so the server page can import it without the hook.
export const GAME_SHELL_PARAM = '_shell';
