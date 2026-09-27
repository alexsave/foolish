import { napkinPng } from '../../lib/kernel-build';

// The napkin as a static file: drawn once, at build, by the kernel's own
// uttt_paper - the sheet every uttt surface sits on - at the 420-point side
// the app stretches over its rules sheet.
export const dynamic = 'force-static';

export function GET() {
    return new Response(new Uint8Array(napkinPng(420)), {
        headers: { 'Content-Type': 'image/png', 'Cache-Control': 'public, max-age=86400' },
    });
}
