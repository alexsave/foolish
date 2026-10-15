/** @type {import('next').NextConfig} */

// Cross-origin isolation, for the Infinite Oracle's Mode B (shared-memory wasm
// threads, docs/INFINITE_ORACLE_DESIGN.md §8b.2). SharedArrayBuffer - and so
// `crossOriginIsolated`, and so Mode B - exists only on a document served with
// COOP: same-origin + COEP: credentialless. Without them the oracle loader
// silently picks Mode A, and Mode B is dead code in the browser.
//
// It is OFF by default, and it is a build-time environment flag rather than a
// code change, because its blast radius is the WHOLE app, not the replay route:
//
//   - Google Fonts. src/app/layout.tsx pulls a stylesheet from
//     fonts.googleapis.com and its faces from fonts.gstatic.com. `credentialless`
//     loads no-credential cross-origin subresources without CORP, and Google
//     serves both with permissive CORS, so this should hold - but "should" is
//     the operative word and it is a live site's typography. (§8b.2 asserts this
//     app has "no CDN fonts". That stopped being true.)
//   - Supabase REST and realtime are CORS fetches and websockets, which COEP
//     permits.
//   - COOP: same-origin severs window.opener. Redirect-based auth is fine; any
//     popup flow is not.
//
// Turning it on is FOOLISH_CROSS_ORIGIN_ISOLATION=1 in the deployment's
// environment plus a redeploy. On CloudFront the same two headers come from
// the web stack instead (c_base/ops/web.yaml, CrossOriginIsolation=yes), since
// a static export answers no headers of its own. Turning it off is unsetting it plus a redeploy:
// no revert, no code review, and the loader returns every visitor to Mode A by
// itself. Verify on the deployment, not from a checkout: open a replay and check
// that `crossOriginIsolated === true` and `window.__oracleMode === 'B'`.
const CROSS_ORIGIN_ISOLATED = process.env.FOOLISH_CROSS_ORIGIN_ISOLATION === '1';

// A LOCAL next.config.js SHADOWS THIS FILE. Next resolves .js before .mjs, and
// this repo's .gitignore hides a next.config.js, so a machine that has one gets
// NONE of this - no rewrites, no headers - while CI and Vercel get all of it.
// The symptom is /privacy, /imessage-privacy and /support falling through to the
// [game_id] route locally and working fine in production. If that happens, move
// the local next.config.js aside rather than doubting this file.
//
// TWO HOSTS, ONE APP, until foolish.cards' DNS moves (c_base/RUNBOOK.md, "Web
// hosting"). By default the build is a static export (out/), which
// c_base/ops/web_deploy.sh uploads to S3 behind CloudFront; there the
// rewrites below and the [game_id] fallback live in the distribution's
// viewer-request Function and the headers in the stack (c_base/ops/web.yaml),
// because an export has no server. Vercel's build (VERCEL=1, which Vercel sets
// and web.yml also sets explicitly) keeps the server build and this routing, so
// both hosts serve the same site while both are live. Delete the Vercel half
// with the Vercel project.
const VERCEL = process.env.VERCEL === '1';

const vercelRouting = {
  async rewrites() {
    return [
      // The iMessage app's App Store privacy policy URL. A static file rather
      // than a React route: every route renders behind KernelGate (a wasm
      // fetch), and this page must load even when the game bundle doesn't.
      { source: '/imessage-privacy', destination: '/imessage-privacy.html' },
      // The web/iOS app's policy, static for the same reason. Separate page
      // because it is a different product with a different answer: that one
      // collects nothing, this one describes an optional account.
      { source: '/privacy', destination: '/privacy.html' },
      // The Support URL on both app records, static for the same reason. It was
      // the other half of the same finding (docs/APP_REVIEW_NOTES.md): a
      // reviewer's `curl` of the React route returned only the noscript line.
      { source: '/support', destination: '/support.html' },
      // The link unfurl is a c_base route now (c_base/docs/VERCEL.md section
      // 4a); on CloudFront /m/* goes to the API's Lambda directly.
      { source: '/m/:payload', destination: 'https://api.foolish.cards/m/:payload' },
    ];
  },
  async headers() {
    if (!CROSS_ORIGIN_ISOLATED) return [];
    return [
      {
        source: '/:path*',
        headers: [
          { key: 'Cross-Origin-Opener-Policy', value: 'same-origin' },
          { key: 'Cross-Origin-Embedder-Policy', value: 'credentialless' },
        ],
      },
    ];
  },
};

const nextConfig = {
  reactStrictMode: true,
  ...(VERCEL ? vercelRouting : { output: 'export' }),
};

export default nextConfig;
