// Ad-hoc Playwright capture of the Infinite Oracle replay overlay
// (docs/INFINITE_ORACLE_DESIGN.md §12.3). Not part of the app build; run against
// `npm run dev` with the pre-provisioned Chromium.
import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';

const CHROME = process.env.ORACLE_CHROME
    || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome';
const BASE = process.env.ORACLE_BASE || 'http://localhost:3000';
const OUT = process.env.ORACLE_OUT || 'docs/screenshots';
const TUTORIAL = 'ENSCBI2LBAVUBJJ3J7NODALIBDGEQYLLLICQ';
const EIGHT = process.env.ORACLE_8WAY;   // 4v4 sample code (passed in)

fs.mkdirSync(OUT, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function jumpTo(page, idx) {
    await page.$eval('input[type=range]', (el, v) => {
        const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
        setter.call(el, String(v));
        el.dispatchEvent(new Event('input', { bubbles: true }));
    }, idx);
}
async function openOracle(page) {
    await page.click('[data-testid="oracle-btn-wrap"] button');
}
async function shot(page, name) {
    const p = path.join(OUT, name);
    await page.screenshot({ path: p });
    console.log('  wrote', p);
}
async function readPanel(page) {
    return await page.$eval('[data-testid="oracle-panel"]', (el) => el.innerText).catch(() => '(no panel)');
}

async function run() {
    const browser = await chromium.launch({ executablePath: CHROME, args: ['--no-sandbox'] });
    const page = await browser.newPage({ viewport: { width: 1280, height: 860 } });
    page.on('console', (m) => { if (m.type() === 'error') console.log('  [console.error]', m.text()); });
    page.on('pageerror', (e) => console.log('  [pageerror]', e.message));

    // ---- tutorial 3-player: mid-game focus sequence --------------------
    console.log('tutorial 3p:', `${BASE}/${TUTORIAL}`);
    await page.goto(`${BASE}/${TUTORIAL}`, { waitUntil: 'load' });
    await page.waitForSelector('input[type=range]', { timeout: 30000 });
    await sleep(1500);                          // opening deal settles

    await jumpTo(page, 28);                      // mid-game cover decision
    await sleep(800);
    await openOracle(page);
    await sleep(450);                            // early: few worlds, wide error bars
    await shot(page, 'oracle-1-open.png');
    console.log('  panel@open:', JSON.stringify(await readPanel(page)).slice(0, 220));
    await sleep(1400);                           // focusing: worlds climbing, bars shrinking
    await shot(page, 'oracle-2-focusing.png');
    console.log('  panel@focus:', JSON.stringify(await readPanel(page)).slice(0, 220));
    await sleep(2200);                           // converged: sharp, ~200k worlds
    await shot(page, 'oracle-3-sharp.png');
    console.log('  panel@sharp:', JSON.stringify(await readPanel(page)).slice(0, 220));

    // ---- exact endgame verdict -----------------------------------------
    await jumpTo(page, 66);                      // heads-up, deck empty, proven
    await sleep(6000);
    await shot(page, 'oracle-4-exact.png');
    console.log('  panel@exact:', JSON.stringify(await readPanel(page)).slice(0, 220));

    // ---- memory OFF resets + re-accumulates ----------------------------
    await page.click('[data-testid="oracle-memory"]');
    await sleep(5000);
    await shot(page, 'oracle-5-memory-off.png');
    console.log('  panel@memoff:', JSON.stringify(await readPanel(page)).slice(0, 220));

    // ---- 8-player big-menu smoke test ----------------------------------
    if (EIGHT) {
        console.log('8-player:', `${BASE}/${EIGHT}`);
        await page.goto(`${BASE}/${EIGHT}`, { waitUntil: 'load' });
        await page.waitForSelector('input[type=range]', { timeout: 30000 });
        await sleep(1500);
        // jump ~40% through
        const max = await page.$eval('input[type=range]', (el) => Number(el.max));
        await jumpTo(page, Math.floor(max * 0.4));
        await sleep(800);
        await openOracle(page);
        await sleep(6000);
        await shot(page, 'oracle-6-8way.png');
        console.log('  panel@8way:', JSON.stringify(await readPanel(page)).slice(0, 220));
    }

    await browser.close();
    console.log('done.');
}
run().catch((e) => { console.error(e); process.exit(1); });
