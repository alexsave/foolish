// Prints the page's DOM after load (debug aid).
import { launch, serve, FOOLISH } from './cdp.mjs';
const server = await serve(8720, FOOLISH);
const b = await launch();
try {
  await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html' + (process.argv[2] || ''));
  await b.until(`document.querySelector('[data-game-container]')`);
  console.log(await b.evaluate(`document.getElementById('root').innerHTML.replace(/></g, '>\\n<')`));
} finally { await b.close(); server.close(); }
