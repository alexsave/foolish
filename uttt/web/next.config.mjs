/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
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
