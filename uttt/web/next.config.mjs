/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  // The iMessage app's privacy policy is a plain static file
  // (public/privacy-msg.html), not a route: the App Store's Privacy Policy URL
  // must read with nothing loading.
  async rewrites() {
    return [{ source: '/privacy-msg', destination: '/privacy-msg.html' }];
  },
  // The kernel is fetched by the page, never bundled: it changes only when the
  // C does, so it can be cached hard between deploys of the same build.
  async headers() {
    return [
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

export default nextConfig;
