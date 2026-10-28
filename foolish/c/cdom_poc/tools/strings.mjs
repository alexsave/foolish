// The string table and the live node ids over three games played out by clicks: after the first
// paint and after each game's end screen. Interned strings are held twice (C's pool and the
// shim's array), so a table that only grows is a leak; it must stay flat once each kind of screen
// has been seen. A game can still show a literal for the first time (an end title, a pill), and
// the games differ from run to run, so game 3 may add over game 2 at most what those literals
// hold: 10 strings and 200 bytes (3 end titles, 5 pill labels, 2 medals: 66 bytes). A leak adds a
// string per new style (the "styles interned" mutant: 14 strings and 6 KB from game 2 to 3).
//   node tools/strings.mjs        (after ./build.sh)
import { launch, serve, FOOLISH, openTable } from './cdp.mjs';
import { playToEnd } from './play.mjs';

const server = await serve(8720, FOOLISH);
const b = await launch();
const rows = [];
try {
  // the test build with the bot's waits 30 ms (the kernel's 3000 otherwise)
  await openTable(b, 'http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug', { fast: true });
  const read = async (when) => rows.push({ when, ...(await b.evaluate(`window.app.then((x) => ({ strings: x.app_str_count(), bytes: x.app_str_bytes(), ids: x.app_ids_live() }))`)) });
  await read('first paint');
  for (let g = 1; g <= 3; g++) {
    const moves = await playToEnd(b);
    await read(`game ${g} over (${moves} moves)`);
    await b.click('[data-end] button');
    await b.until(`document.querySelector('[data-game-container]').dataset.turn !== 'bot' && !document.querySelector('[data-end]')`, 15000);
  }
} finally { await b.close(); server.close(); }
for (const r of rows) console.log(`${r.when.padEnd(26)} ${String(r.strings).padStart(5)} strings ${String(r.bytes).padStart(7)} bytes ${String(r.ids).padStart(5)} live ids`);
process.exit(rows[3].strings - rows[2].strings > 10 || rows[3].bytes - rows[2].bytes > 200 ? 1 : 0);
