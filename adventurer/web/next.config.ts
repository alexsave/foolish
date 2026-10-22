import type { NextConfig } from 'next';

// The page is client-rendered where it talks to the C server: the browser fetches the binary
// endpoints and opens the feed WebSocket against NEXT_PUBLIC_ADV_SERVER directly. Next serves
// the shell only, so nothing here proxies or rewrites /adv.
const nextConfig: NextConfig = {
  reactStrictMode: true,
  // `next dev` refuses its own chunks to a host it does not consider its origin, and
  // 127.0.0.1 is not localhost to it: without this the page served at 127.0.0.1:5711 never
  // hydrates (no error, just a dead shell).
  allowedDevOrigins: ['127.0.0.1'],
  // The dev build's floating "N" button sits over the regions card; the page is the product.
  devIndicators: false,
};

export default nextConfig;
