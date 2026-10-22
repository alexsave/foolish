import type { NextConfig } from 'next';

// The page is client-rendered where it talks to birdseye: the browser fetches the binary
// endpoints and opens the stream against NEXT_PUBLIC_BE_SERVER directly. Next serves the shell
// only, so nothing here proxies or rewrites /be.
const nextConfig: NextConfig = {
  reactStrictMode: true,
  // `next dev` refuses its own chunks to a host it does not consider its origin, and 127.0.0.1 is
  // not localhost to it: without this the page served at 127.0.0.1:5811 never hydrates.
  allowedDevOrigins: ['127.0.0.1'],
  // The site sits inside a monorepo with other lockfiles; its root is this folder.
  turbopack: { root: __dirname },
};

export default nextConfig;
