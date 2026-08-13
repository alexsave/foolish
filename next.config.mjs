/** @type {import('next').NextConfig} */
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
};

export default nextConfig;
