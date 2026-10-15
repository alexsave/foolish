// The one [code] page the static export writes (out/_shell.html). The site is
// served from S3 behind CloudFront (c_base/ops/web.yaml), where the viewer-request
// Function answers every unknown extensionless path with this file, so the code
// a visitor asked for is in location.pathname, not in the route param the page
// was prerendered with. A server build (Vercel, until DNS moves) renders the real
// param instead and never sees this value.
export const SHELL_CODE = '_shell';
