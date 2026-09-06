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
// environment plus a redeploy. Turning it off is unsetting it plus a redeploy:
// no revert, no code review, and the loader returns every visitor to Mode A by
// itself. Verify on the deployment, not from a checkout: open a replay and check
// that `crossOriginIsolated === true` and `window.__oracleMode === 'B'`.
const CROSS_ORIGIN_ISOLATED = process.env.FOOLISH_CROSS_ORIGIN_ISOLATION === '1';

const nextConfig = {
  reactStrictMode: true,
  async rewrites() {
    return [
      // The iMessage app's App Store privacy policy URL. A static file rather
      // than a React route: every route renders behind KernelGate (a wasm
      // fetch), and this page must load even when the game bundle doesn't.
      { source: '/imessage-privacy', destination: '/imessage-privacy.html' },
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

export default nextConfig;
