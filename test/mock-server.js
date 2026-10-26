#!/usr/bin/env node
// A scripted Ollama and OpenAI-compatible server for the VICE tests
// (test/run.py). It answers the requests SkinnyAI 64 makes, streaming
// replies a few words at a time, and appends each request it gets to a
// log, one JSON object a line, so the tests can check what was sent.
//
//   node test/mock-server.js [port] [log file]     (default 11500, no log)
//
// What a chat reply says depends on the last user message:
//   ...markdown...  a reply with a heading, bold, code, a list, a table
//   ...lines...     40 numbered lines, for the pager
//   ...chunked...   chunked encoding split at odd places, with UTF-8 and
//                   \u escapes (as test/chunked-server.js)
//   ...think...     thinking first, then the answer
//   ...nomodel...   a 404 with Ollama's "model not found" error
//   ...hangup...    the connection closed without an answer
//   ...search...    (a -cloud model with tools) calls web_search, then
//                   fetch_page on a long URL, then answers with what the
//                   fetch returned
//   ...forever...   (a -cloud model with tools) calls web_search every time
//                   it's offered tools
//   anything else   "Reply <n> to <text>.", n counting the messages sent
// /api/experimental/web_search and web_fetch answer as a signed-in Ollama
// does, and a search for "...fail..." gets an error.
const net = require('net');
const fs = require('fs');

const PORT = Number(process.argv[2] || 11500);
const LOG = process.argv[3];

const MARKDOWN = '# Heading\nSome **bold** text and `code`.\n\n- first item\n- second item\n\n| Name | Value |\n|---|---|\n| alpha | 1 |\n';
const CHUNKED = ['Caf\u00e9 ', '\u201cquoted\u201d ', 'dash \u2014 ', 'emoji \ud83d\ude00 ', 'done.'];

function words(text) {
  return text.match(/\S+\s*|\s+/g) || [];
}

function lastUser(body) {
  const msgs = (body && body.messages) || [];
  for (let i = msgs.length - 1; i >= 0; --i) if (msgs[i].role === 'user') return msgs[i].content;
  return '';
}

const LONG_URL = 'https://example.com/a/rather/long/path/to/a/page/about/the/commodore/64/news.html';
const PAGE_TEXT = 'The   C64\n\n\n  is   back.\t' + 'x'.repeat(3000);

function replyText(body) {
  const text = lastUser(body);
  const msgs = body.messages;
  const last = msgs[msgs.length - 1];
  if (last.role === 'tool') return `Read ${msgs.length}: ${last.content.slice(0, 20).replace(/\n/g, '/')}`;
  if (/markdown/i.test(text)) return MARKDOWN;
  if (/lines/i.test(text)) return Array.from({ length: 40 }, (_, i) => `Line ${i + 1}`).join('\n') + '\n';
  return `Reply ${body.messages.length} to ${text.replace(/[.?!]+$/, '')}.`;
}

// One streamed piece of a reply in the API's own framing.
function ollamaLine(model, fields, done) {
  return JSON.stringify({ model, created_at: '2026-01-01T00:00:00Z', message: { role: 'assistant', content: '', ...fields }, done }) + '\n';
}

function openaiLine(model, delta, finish) {
  return 'data: ' + JSON.stringify({ id: 'chatcmpl-1', object: 'chat.completion.chunk', model, choices: [{ index: 0, delta, finish_reason: finish || null }] }) + '\n\n';
}

function chatPieces(req) {
  const { body } = req;
  const openai = req.path.startsWith('/v1/');
  const model = body.model;
  const pieces = [];
  const text = lastUser(body);
  const last = body.messages[body.messages.length - 1];
  if (!openai && body.tools && /search|forever/i.test(text)) {
    let fn = null;
    if (/forever/i.test(text) || last.role === 'user') {
      fn = { name: 'web_search', arguments: { query: /fail/i.test(text) ? 'fail please' : 'c64 news' } };
      if (/search/i.test(text)) pieces.push(ollamaLine(model, { content: 'Looking.' }, false));
    } else if (last.tool_name === 'web_search' && !/fail/i.test(text)) {
      fn = { name: 'fetch_page', arguments: { url: LONG_URL } };
    }
    if (fn) {
      pieces.push(ollamaLine(model, { tool_calls: [{ id: 'call_1', function: { index: 0, ...fn } }] }, false));
      pieces.push(ollamaLine(model, {}, true));
      return pieces;
    }
  }
  if (/think/i.test(text)) {
    for (const w of words('Let me think about it.')) {
      pieces.push(openai ? openaiLine(model, { reasoning_content: w }) : ollamaLine(model, { thinking: w }, false));
    }
  }
  for (const w of words(replyText(body))) {
    pieces.push(openai ? openaiLine(model, { content: w }) : ollamaLine(model, { content: w }, false));
  }
  if (openai) {
    pieces.push(openaiLine(model, {}, 'stop'));
    pieces.push('data: [DONE]\n\n');
  } else {
    pieces.push(JSON.stringify({ model, message: { role: 'assistant', content: '' }, done: true, done_reason: 'stop',
      total_duration: 1500000000, prompt_eval_count: 12, eval_count: 7, eval_duration: 1000000000 }) + '\n');
  }
  return pieces;
}

function send(sock, status, type, pieces) {
  const reason = { 200: 'OK', 404: 'Not Found' }[status] || 'Error';
  sock.write(`HTTP/1.1 ${status} ${reason}\r\nContent-Type: ${type}\r\nDate: Mon, 26 Oct 2026 12:00:00 GMT\r\nConnection: close\r\n\r\n`);
  // a little apart, as a real server streams them
  let i = 0;
  const next = () => {
    if (i < pieces.length) {
      sock.write(pieces[i++]);
      setTimeout(next, 5);
    } else {
      sock.end();
    }
  };
  next();
}

function sendJson(sock, status, obj) {
  const body = Buffer.from(JSON.stringify(obj));
  sock.write(`HTTP/1.1 ${status} OK\r\nContent-Type: application/json\r\nContent-Length: ${body.length}\r\n\r\n`);
  sock.end(body);
}

function sendChunked(sock, model) {
  const lines = CHUNKED.map((w) => JSON.stringify({ model, message: { role: 'assistant', content: w }, done: false }) + '\n');
  lines.push('{"model":"mock","message":{"role":"assistant","content":" \\u00bdcup"},"done":false}\n');
  lines.push(JSON.stringify({ model, message: { role: 'assistant', content: '' }, done: true }) + '\n');
  const body = Buffer.from(lines.join(''), 'utf8');
  sock.write('HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nTransfer-Encoding: chunked\r\n\r\n');
  const sizes = [7, 1, 50, 13, 200, 3];
  for (let i = 0, k = 0; i < body.length; ) {
    const n = Math.min(sizes[k++ % sizes.length], body.length - i);
    sock.write(n.toString(16) + ';ext=1\r\n');
    sock.write(body.subarray(i, i + n));
    sock.write('\r\n');
    i += n;
  }
  sock.end('0\r\n\r\n');
}

function answer(sock, req) {
  const { method, path, body } = req;
  if (method === 'GET' && path === '/api/tags') {
    return sendJson(sock, 200, { models: [
      { name: 'mock:7b', details: { parameter_size: '7B' } },
      { name: 'tiny:1b', details: { parameter_size: '1B' } },
    ] });
  }
  if (method === 'GET' && path === '/v1/models') {
    return sendJson(sock, 200, { object: 'list', data: [{ id: 'gpt-mock', object: 'model' }, { id: 'gpt-tiny', object: 'model' }] });
  }
  if (method === 'POST' && path === '/api/show') {
    return sendJson(sock, 200, {
      details: { family: 'mockfam', parameter_size: '7B', quantization_level: 'Q4_K_M' },
      model_info: { 'mock.context_length': 8192 },
      capabilities: ['completion', 'tools'],
    });
  }
  if (method === 'POST' && path === '/api/experimental/web_search') {
    if (/fail/.test(body.query)) return sendJson(sock, 401, { error: 'unauthorized' });
    return sendJson(sock, 200, { results: Array.from({ length: body.max_results || 5 }, (_, i) => ({
      title: `Result ${i + 1}`, url: `https://example.com/${i + 1}`, content: `Text   of\n\n\nresult ${i + 1}. ` + 'y'.repeat(1000),
    })) });
  }
  if (method === 'POST' && path === '/api/experimental/web_fetch') {
    return sendJson(sock, 200, { title: 'C64 news', content: PAGE_TEXT, links: [LONG_URL] });
  }
  if (method === 'POST' && (path === '/api/chat' || path === '/v1/chat/completions')) {
    const text = lastUser(body);
    if (/hangup/i.test(text)) return sock.destroy();
    if (/nomodel/i.test(text)) return sendJson(sock, 404, { error: `model "${body.model}" not found, try pulling it first` });
    if (/chunked/i.test(text)) return sendChunked(sock, body.model);
    const openai = path.startsWith('/v1/');
    return send(sock, 200, openai ? 'text/event-stream' : 'application/x-ndjson', chatPieces(req));
  }
  return sendJson(sock, 404, { error: `no route for ${method} ${path}` });
}

net.createServer((sock) => {
  let buf = Buffer.alloc(0);
  sock.on('error', () => {});
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    const end = buf.indexOf('\r\n\r\n');
    if (end < 0) return;
    const head = buf.subarray(0, end).toString('latin1').split('\r\n');
    const headers = {};
    for (const line of head.slice(1)) {
      const i = line.indexOf(':');
      headers[line.slice(0, i).toLowerCase()] = line.slice(i + 1).trim();
    }
    const len = Number(headers['content-length'] || 0);
    if (buf.length < end + 4 + len) return;
    sock.removeAllListeners('data');
    const [method, path, version] = head[0].split(' ');
    const raw = buf.subarray(end + 4, end + 4 + len).toString('utf8');
    let body = null;
    let bodyError = null;
    if (len) {
      try {
        body = JSON.parse(raw);
      } catch (e) {
        bodyError = e.message;
      }
    }
    const req = { method, path, version, headers, body, raw: bodyError ? raw : undefined, bodyError };
    if (LOG) fs.appendFileSync(LOG, JSON.stringify(req) + '\n');
    if (bodyError) return sendJson(sock, 400, { error: 'bad JSON: ' + bodyError });
    answer(sock, req);
  });
}).listen(PORT, '127.0.0.1', () => console.log(`mock server on 127.0.0.1:${PORT}`));
