/** @type {import('next').NextConfig} */

// TWO HOSTS, ONE APP, until uttt.live's DNS moves (c_base/RUNBOOK.md, "Web
// hosting"). By default the build is a static export (out/), which
// c_base/ops/web_deploy.sh uploads to S3 behind CloudFront; there the routing
// below lives in the distribution's viewer-request Function (c_base/ops/web.yaml),
// because an export has no server to run rewrites, redirects or headers.
// Vercel's build (VERCEL=1, which Vercel sets and uttt-web.yml also sets
// explicitly) keeps the server build and the routing here, so the two hosts
// serve the same site while both are live. Delete the Vercel half with the
// Vercel project.
const VERCEL = process.env.VERCEL === '1';

const vercelRouting = {
  // The iMessage app's privacy policy and support page are plain static files
  // (public/*-msg.html), not routes: the App Store's URLs must read with
  // nothing loading.
  async rewrites() {
    return [
      { source: '/privacy-msg', destination: '/privacy-msg.html' },
      { source: '/support-msg', destination: '/support-msg.html' },
    ];
  },
  // The 243 board is 243 across; /234 is the typo everyone makes once.
  async redirects() {
    return [{ source: '/234', destination: '/243', permanent: false }];
  },
  // The kernel is fetched by the page, never bundled: it changes only when the
  // C does, so it can be cached hard between deploys of the same build.
  // (On S3 the deploy sets the same two headers on the objects.)
  async headers() {
    return [
      {
        source: '/uttt243.wasm',
        headers: [
          { key: 'Content-Type', value: 'application/wasm' },
          { key: 'Cache-Control', value: 'public, max-age=3600' },
        ],
      },
      {
        source: '/uttt.wasm',
        headers: [
          { key: 'Content-Type', value: 'application/wasm' },
          { key: 'Cache-Control', value: 'public, max-age=3600' },
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
