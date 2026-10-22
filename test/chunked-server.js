#!/usr/bin/env node
// A deliberately awkward server for testing: answers /api/chat with a
// chunked HTTP/1.1 response (even to HTTP/1.0, as some servers do), the
// NDJSON split at odd places, with \u escapes and UTF-8 in the text.
const net = require('net');
const PORT = Number(process.argv[2] || 11500);

const words = ['Caf\u00e9 ', '\u201cquoted\u201d ', 'dash \u2014 ', 'arrow \u2192 ', 'emoji \ud83d\ude00 ', 'done.'];
const lines = words.map((w) => JSON.stringify({ model: 'mock', message: { role: 'assistant', content: w }, done: false }) + '\n');
// one line with \u escapes as some servers send them
lines.push('{"model":"mock","message":{"role":"assistant","content":" \\u00bdcup \\u2026"},"done":false}\n');
lines.push(JSON.stringify({ model: 'mock', message: { role: 'assistant', content: '' }, done: true, eval_count: 7 }) + '\n');
const body = Buffer.from(lines.join(''), 'utf8');

net.createServer((sock) => {
  let req = '';
  sock.on('data', (d) => {
    req += d.toString('latin1');
    const end = req.indexOf('\r\n\r\n');
    if (end < 0) return;
    const len = Number((/content-length: *(\d+)/i.exec(req) || [0, 0])[1]);
    if (req.length < end + 4 + len) return;
    sock.write('HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nTransfer-Encoding: chunked\r\n\r\n');
    let i = 0;
    const sizes = [7, 1, 50, 13, 200, 3];
    let k = 0;
    while (i < body.length) {
      const n = Math.min(sizes[k++ % sizes.length], body.length - i);
      sock.write(n.toString(16) + ';ext=1\r\n');
      sock.write(body.subarray(i, i + n));
      sock.write('\r\n');
      i += n;
    }
    sock.write('0\r\n\r\n');
    sock.end();
  });
}).listen(PORT, '127.0.0.1');
