// The [poc] network imports in a real browser: a wasm module opens a WebSocket to this
// process, sends 8 binary bytes, expects them echoed, then POSTs them and expects them back
// reversed. The echo server is in this file (a minimal RFC 6455 frame reader). Exits non-zero
// on a failure.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { launch, serve, POC } from '../tools/cdp.mjs';

const out = path.join(POC, 'out');
const shim = fs.readFileSync(path.join(POC, 'vendor/cdom.js'), 'utf8');
const page = `<!DOCTYPE html><html><head><meta charset="utf-8"/><script>${shim}
cdom.start('net_test.wasm', 'root');</script></head><body><div id="root"></div></body></html>`;

const server = await serve(8720, out, (req, res) => {
  if (req.url === '/net.html') { res.writeHead(200, { 'content-type': 'text/html' }); res.end(page); return true; }
  if (req.url === '/echo' && req.method === 'POST') {
    const parts = [];
    req.on('data', (d) => parts.push(d));
    req.on('end', () => { res.writeHead(200, { 'content-type': 'application/octet-stream' }); res.end(Buffer.concat(parts).reverse()); });
    return true;
  }
  return false;
});
server.on('upgrade', (req, sock) => {
  const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
  let buf = Buffer.alloc(0);
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    while (buf.length >= 6) {
      const op = buf[0] & 15;
      let len = buf[1] & 127, at = 2;
      if (len === 126) { len = buf.readUInt16BE(2); at = 4; }
      if (buf.length < at + 4 + len) return;
      const mask = buf.subarray(at, at + 4), body = Buffer.from(buf.subarray(at + 4, at + 4 + len));
      for (let i = 0; i < len; i++) body[i] ^= mask[i & 3];
      buf = buf.subarray(at + 4 + len);
      if (op === 8) { sock.end(Buffer.from([0x88, 0])); return; }
      if (op === 2) sock.write(Buffer.concat([Buffer.from([0x82, len]), body]));   // echo, under 126 bytes
    }
  });
});

const b = await launch();
let verdict = '';
try {
  await b.goto('http://127.0.0.1:8720/net.html');
  verdict = await b.until(`(() => { const t = document.getElementById('status')?.textContent; return t && t !== 'waiting' && t !== 'ws ok' ? t : ''; })()`);
} finally { await b.close(); server.close(); }
console.log(verdict);
process.exit(verdict === 'ws ok, http ok' ? 0 : 1);
