/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  // Cross-origin isolation for the Infinite Oracle's Mode B (shared-memory wasm
  // threads, docs/INFINITE_ORACLE_DESIGN.md §8b.2). SharedArrayBuffer requires
  // COOP:same-origin + COEP:credentialless, which makes `crossOriginIsolated`
  // true so the oracle loader can pick oracle-mt.wasm.gz; where isolation is
  // unavailable it silently falls back to the Mode A instance fleet.
  //
  // Blast radius (kept in mind, §8b.2): these apply to the whole app, not just
  // the replay route. `credentialless` lets cross-origin no-credential
  // subresources load without CORP (Google Fonts, @vercel/analytics), and
  // Supabase REST/realtime are CORS fetches/websockets that COEP permits. If an
  // OAuth popup flow breaks under COOP, ROLLBACK is trivial: delete this
  // headers() block and the loader returns everyone to Mode A automatically.
  async headers() {
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
