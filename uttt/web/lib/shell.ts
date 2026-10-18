// The one [code] page the static export writes (out/_shell.html). The site is
// served from S3 behind CloudFront (c_base/providers/aws/ops/site/web.yaml), where the viewer-request
// Function answers every unknown extensionless path with this file, so the code
// a visitor asked for is in location.pathname, not in the route param the page
// was prerendered with.
export const SHELL_CODE = '_shell';
