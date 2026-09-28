/* =============================================================================
 * The probe that measures what `run_e2e.mjs --write-order` writes down
 * =============================================================================
 * Preloaded into every per-file child ONLY when the runner is refreshing
 * e2e/FILE_ORDER. It asserts nothing and prints nothing: at exit it appends one
 * `<file> <elapsed_ms> <cpu_ms>` line to $E2E_ORDER_OUT.
 *
 * Both numbers, because they answer different questions and the order file is
 * ranked on one of them:
 *
 *   elapsed - what the file occupies a slot for. That is what a makespan is made
 *             of, but on a busy machine it also contains every second the file
 *             spent queued behind seven other files for a core, so it is partly
 *             a measurement of the run that took it.
 *   cpu     - process.cpuUsage(), user + system, the work the file actually did.
 *             Reproducible across runs and across machines with different core
 *             counts, and therefore the honest RANKING key.
 *
 * The two disagree exactly where it matters: a db-lane file that spends its life
 * waiting on Postgres has small cpu and large elapsed. Ranking on cpu alone
 * would start those last; ranking on elapsed alone re-measures the contention of
 * whichever run happened to write the file. The runner ranks on cpu and keeps
 * elapsed beside it in the file, so a reader can see which files are waiting
 * rather than working - see scripts/run_e2e.mjs.
 *
 * A node:test reporter cannot do this job: it runs in the PARENT, and the parent
 * cannot see a child's cpuUsage. Hence a preload rather than a reporter, which
 * also leaves node's default reporter (and so the suite's output) untouched.
 * ========================================================================== */
import { appendFileSync } from 'node:fs';

const out = process.env.E2E_ORDER_OUT;
if (out) {
    const file = process.argv[1];
    const t0 = Date.now();
    process.on('exit', () => {
        const cpu = process.cpuUsage();
        try {
            appendFileSync(out, `${file} ${Date.now() - t0} ${Math.round((cpu.user + cpu.system) / 1000)}\n`);
        } catch { /* a measurement that fails must not fail the run */ }
    });
}
