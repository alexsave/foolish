/** @type {import('next').NextConfig} */
// A static export (out/): the c_chess server reads it into memory at start (CC_WEB_DIR) and
// serves it beside the API, so the site and the socket are one origin and one process.
const nextConfig = {
  reactStrictMode: true,
  output: 'export',
  poweredByHeader: false,
};
export default nextConfig;
