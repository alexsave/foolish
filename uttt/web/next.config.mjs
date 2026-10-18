/** @type {import('next').NextConfig} */

// The build is a static export (out/), which c_base/ops/web_deploy.sh uploads
// to S3 behind CloudFront. An export has no server to run rewrites, redirects
// or headers, so the routing (/privacy-msg, /support-msg, /234 -> /243) and
// the wasm headers live in the distribution's viewer-request Function and the
// stack (c_base/ops/web.yaml).
const nextConfig = {
  reactStrictMode: true,
  output: 'export',
};

export default nextConfig;
