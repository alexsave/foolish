/* One Mode A fleet member (scripts/oracle_bench.mts): its own private
 * oracle.wasm instance, driven through the SAME bridge the replay screen's
 * workers use (src/oracle/oracleBridge.ts), so what the bench times per batch
 * is what a browser pays per batch - import state, strategy keys, logs, seed,
 * choose, read the answer - and not a re-typed copy of it that can fall behind
 * the kernel's exports. (It had: the copy this replaced still called the state
 * import with its old one-argument signature, was refused every batch, and
 * measured nothing.) The bench runs under `node --import tsx`, which a worker
 * inherits, which is what lets this file import TypeScript. */
import { parentPort, workerData } from 'node:worker_threads';
import { OracleInstance } from '../../src/oracle/oracleBridge.ts';

const { bytes, job, knobs, seconds, tid } = workerData;

const inst = new OracleInstance();
await inst.init(bytes);
inst.setKnobs(knobs);

let chooses = 0, seed = 0x1234 + tid * 7919;
const end = Date.now() + seconds * 1000;
while (Date.now() < end) {
    for (let k = 0; k < 8; k++) {                     // batch the clock check
        seed = (seed * 1103515245 + 12345) >>> 0;
        if ('record' in inst.analyzeOnce(job, seed)) chooses++;
    }
}
parentPort.postMessage({ chooses });
