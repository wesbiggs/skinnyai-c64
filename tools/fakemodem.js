#!/usr/bin/env node
// A stand-in for the TeensyROM's ethernet modem, for testing in VICE.
//
// VICE's emulated SwiftLink connects to this as its RS232 device (see
// tools/run-vice.sh); it answers AT commands the way the TeensyROM firmware
// does (Swift_ATcommands.c): words in ASCII ending in CR, informational text
// in PETSCII, echo and verbose on at start, ATDT host:port opening a real
// TCP connection, "NO CARRIER" when the server hangs up, and "+++" with a
// second's quiet on either side hanging up early.
//
// VICE doesn't pass RTS flow control over TCP, so bytes to the C64 are paced
// (FAKEMODEM_BPS, default 2400 bytes/s) instead of held when it's busy.
// ATC answers after FAKEMODEM_ATC_DELAY ms (default 2000), as DHCP might.
//
//   node tools/fakemodem.js [port]      (default 25232)

const net = require('net');

const PORT = Number(process.argv[2] || 25232);
const BPS = Number(process.env.FAKEMODEM_BPS || 2400);
const ATC_DELAY = Number(process.env.FAKEMODEM_ATC_DELAY ?? 2000); // ms, as if waiting for DHCP
const log = (...a) => process.env.FAKEMODEM_QUIET || console.log('[modem]', ...a);

// ASCII text as the firmware's ToPETSCII sends it.
const petscii = (s) =>
  Buffer.from([...s].map((ch) => {
    const c = ch.charCodeAt(0);
    if (c >= 0x61 && c <= 0x7a) return c - 0x20;
    if (c >= 0x41 && c <= 0x5a) return c + 0x80;
    return c;
  }));

net.createServer((c64) => {
  log('C64 connected');
  let echo = true;
  let verbose = true;
  let cmd = '';
  let remote = null;
  let lastTx = 0;
  let plus = 0;
  let plusTimer = null;

  // Paced output to the C64.
  const queue = [];
  let sending = false;
  const pump = () => {
    if (!queue.length) {
      sending = false;
      return;
    }
    sending = true;
    const n = Math.max(1, Math.round(BPS / 100));
    const chunk = Buffer.concat(queue.splice(0)).subarray(0);
    const now = chunk.subarray(0, n);
    const rest = chunk.subarray(n);
    if (rest.length) queue.unshift(rest);
    c64.write(now);
    setTimeout(pump, 10);
  };
  const send = (buf) => {
    queue.push(Buffer.isBuffer(buf) ? buf : Buffer.from(buf, 'latin1'));
    if (!sending) pump();
  };
  const result = (word, code) => send(verbose ? word + '\r' : petscii(String(code) + '\r'));
  const info = (text) => send(Buffer.concat([petscii(text), Buffer.from('\r')]));

  const hangup = (why) => {
    if (!remote) return;
    const r = remote;
    remote = null;
    r.destroy();
    log('hung up:', why);
  };

  const dial = (arg) => {
    let [host, port] = arg.replace(/"/g, '').split(':');
    port = Number(port || 6400);
    send(petscii(`Trying "${host}"\r\n on port ${port}...`));
    send('\r');
    const sock = net.connect({ host, port });
    let connected = false;
    sock.on('connect', () => {
      connected = true;
      remote = sock;
      log('connected to', host, port);
      result('CONNECT', 1);
    });
    sock.on('data', (d) => {
      if (remote === sock) send(d);
    });
    sock.on('error', (e) => {
      if (!connected) {
        log('dial failed:', e.message);
        result('NO ANSWER', 8);
      }
    });
    sock.on('close', () => {
      if (!connected) return;
      if (remote === sock) {
        remote = null;
        log('server closed the connection');
      }
      result('NO CARRIER', 3);
    });
  };

  const command = (line) => {
    const lc = line.toLowerCase().trim();
    log('AT command:', JSON.stringify(lc));
    if (!lc.startsWith('at')) return result('ERROR', 4);
    const rest = lc.slice(2).trimStart();
    if (rest === '' || rest === 'h' || rest.startsWith('z')) return result('OK', 0);
    if (rest.startsWith('e') || rest.startsWith('v')) {
      const v = rest.slice(1).trim();
      if (v !== '0' && v !== '1') {
        info('Invalid Format');
        return result('ERROR', 4);
      }
      if (rest[0] === 'e') echo = v === '1';
      else verbose = v === '1';
      return result('OK', 0);
    }
    if (rest === 'c') {
      setTimeout(() => {
        info('MAC Address: BE:0C:64:C0:FF:EE');
        info('Local IP: 10.0.0.64');
        info('Subnet Mask: 255.255.255.0');
        info('Gateway IP: 10.0.0.1');
        result('OK', 0);
      }, ATC_DELAY);
      return;
    }
    if (rest.startsWith('dt')) return dial(rest.slice(2).trim());
    info('unknown command: ' + lc);
    return result('ERROR', 4);
  };

  c64.on('data', (data) => {
    for (const b of data) {
      if (remote) {
        // +++ needs a second's quiet before the first + and after the third
        const now = Date.now();
        if (b === 0x2b && (plus || now - lastTx > 1000)) {
          plus++;
          clearTimeout(plusTimer);
          if (plus === 3) {
            plusTimer = setTimeout(() => {
              plus = 0;
              hangup('+++');
              info('*aborted by user*');
            }, 1000);
          }
        } else {
          plus = 0;
          clearTimeout(plusTimer);
        }
        lastTx = now;
        remote.write(Buffer.from([b]));
        continue;
      }
      if (echo) send(Buffer.from([b]));
      if (b === 0x0d) {
        if (cmd.length) command(cmd);
        cmd = '';
      } else if (b === 0x08 || b === 0x14) {
        cmd = cmd.slice(0, -1);
      } else {
        cmd += String.fromCharCode(b);
        if (cmd.length >= 128) {
          command(cmd);
          cmd = '';
        }
      }
    }
  });
  c64.on('close', () => {
    hangup('C64 went away');
    log('C64 disconnected');
  });
  c64.on('error', () => {});
}).listen(PORT, '127.0.0.1', () => log(`listening on 127.0.0.1:${PORT}, ${BPS} bytes/s`));
