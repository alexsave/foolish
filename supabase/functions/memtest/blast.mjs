// Concurrency blast against the local edge runtime: N simultaneous requests,
// each running a couple of semtex/octogen decisions. Reports status counts and
// latency percentiles.
const N = Number(process.argv[2] ?? 100);
const URL = 'http://127.0.0.1:54321/functions/v1/memtest?keys=semtex,octogen&maxmoves=2';

const t0 = performance.now();
const results = await Promise.all(Array.from({ length: N }, async () => {
    const t = performance.now();
    try {
        const r = await fetch(URL);
        await r.text();
        return { code: r.status, ms: performance.now() - t };
    } catch (e) {
        return { code: 0, ms: performance.now() - t };
    }
}));
const wall = performance.now() - t0;

const byCode = {};
for (const r of results) byCode[r.code] = (byCode[r.code] ?? 0) + 1;
const lat = results.filter(r => r.code === 200).map(r => r.ms).sort((a, b) => a - b);
const pct = (p) => lat.length ? Math.round(lat[Math.min(lat.length - 1, Math.floor(p * lat.length))]) : -1;
console.log(JSON.stringify({
    N, wall_s: +(wall / 1000).toFixed(1), codes: byCode,
    ok_latency_ms: { p50: pct(0.5), p90: pct(0.9), p99: pct(0.99), max: Math.round(lat.at(-1) ?? -1) },
    throughput_rps: +(results.length / (wall / 1000)).toFixed(1),
}));
