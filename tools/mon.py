#!/usr/bin/env python3
"""Drives SkinnyAI 64 running in VICE (tools/run-vice.sh) through VICE's
remote monitor: read the screen, type, wait for text.

  tools/mon.py screen                  print the 40x25 screen as text
  tools/mon.py type 'hello\\r'          type (\\r = RETURN, \\n = SHIFT+RETURN,
                                       \\s = RUN/STOP, \\u \\d \\l \\R = cursor
                                       up/down/left/right, \\x = DEL,
                                       \\c = CLR, \\h = HOME)
  tools/mon.py wait 'text' [seconds]   wait until text is on screen
  tools/mon.py peek c6                 a byte of memory (hex address)
  tools/mon.py shot file.png           screenshot
  tools/mon.py quit                    quit VICE
"""
import re
import socket
import sys
import time

ADDR = ('127.0.0.1', 6510)
PROMPT = re.compile(rb'\(C:\$[0-9a-f]{4}\) ')


class Monitor:
    def __init__(self):
        self.s = socket.create_connection(ADDR, timeout=10)

    def wait_prompt(self):
        # Entering the monitor prints a prompt before the command's output,
        # so read until a prompt ends the text and nothing more follows.
        buf = b''
        while True:
            chunk = self.s.recv(65536)
            if not chunk:
                return buf
            buf += chunk
            if PROMPT.search(buf[-12:]):
                self.s.settimeout(0.2)
                try:
                    more = self.s.recv(65536)
                except socket.timeout:
                    more = b''
                self.s.settimeout(10)
                if not more:
                    return buf
                buf += more

    def cmd(self, c):
        self.s.sendall(c.encode() + b'\n')
        return PROMPT.sub(b'', self.wait_prompt()).decode('latin1')

    def resume(self):
        self.s.sendall(b'x\n')
        time.sleep(0.05)

    def read(self, start, end):
        out = self.cmd('m %04x %04x' % (start, end))
        data = []
        want = end - start + 1
        for line in out.splitlines():
            line = line.strip()
            if not line.startswith('>C:'):
                continue
            n = min(16, want - len(data))
            data += [int(x, 16) for x in line[3:].split()[1:1 + n]]
        return data[:want]


_monitor = None


def monitor():
    """The connection to VICE, opened once and kept. VICE can stall writing
    to a connection that has gone away, so one is reused, not one a read."""
    global _monitor
    if _monitor is None:
        _monitor = Monitor()
    return _monitor


def disconnect():
    global _monitor
    if _monitor is not None:
        _monitor.s.close()
        _monitor = None


def screen_code_to_ascii(c):
    c &= 0x7F
    if c == 0: return '@'
    if 1 <= c <= 26: return chr(c + 96)
    if c == 0x1B: return '['
    if c == 0x1C: return '\\'
    if c == 0x1D: return ']'
    if c == 0x1E: return '^'
    if c == 0x1F: return '_'
    if 0x20 <= c <= 0x3F: return chr(c)
    if 0x41 <= c <= 0x5A: return chr(c)
    return {0x7A: '`', 0x7B: '{', 0x7C: '}', 0x7E: '~', 0x5D: '|', 0x40: '-', 0x6D: '~'}.get(c, '#')


def screen(m):
    data = m.read(0x0400, 0x07E7)
    return '\n'.join(''.join(screen_code_to_ascii(c) for c in data[r * 40:(r + 1) * 40]).rstrip()
                     for r in range(25))


PALETTE = [(0, 0, 0), (255, 255, 255), (136, 57, 50), (103, 182, 189), (139, 63, 150),
           (85, 160, 73), (64, 49, 141), (191, 206, 114), (139, 84, 41), (87, 66, 0),
           (184, 105, 98), (80, 80, 80), (120, 120, 120), (148, 224, 137), (120, 105, 196),
           (159, 159, 159)]


def render(chars, scr, col, bg):
    """The text screen as 320x200 RGB rows, scaled 2x."""
    rows = []
    for y in range(200):
        row = []
        for x in range(320):
            o = (y // 8) * 40 + x // 8
            bits = chars[scr[o] * 8 + y % 8]
            on = bits & (0x80 >> (x % 8))
            c = PALETTE[(col[o] & 15) if on else (bg & 15)]
            row += [c, c]
        rows.append(row)
        rows.append(row)
    return rows


def write_png(path, rows):
    import struct, zlib
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\x00' + bytes(v for px in r for v in px) for r in rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def petscii(text):
    out = []
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == '\\' and i + 1 < len(text):
            nxt = text[i + 1]
            i += 2
            out.append({'r': 13, 'n': 141, 's': 3, 'u': 145, 'd': 17, 'l': 157, 'R': 29,
                        'x': 20, 'c': 147, 'h': 19, '\\': 0x5C}[nxt])
            continue
        c = ord(ch)
        if 0x61 <= c <= 0x7A: c -= 0x20
        elif 0x41 <= c <= 0x5A: c += 0x80
        elif ch == '{': c = 0xDB
        elif ch == '}': c = 0xDD
        elif ch == '|': c = 0xA9
        elif ch == '~': c = 0xBA
        elif ch == '`': c = 0xC0
        out.append(c)
        i += 1
    return out


def type_text(text):
    codes = petscii(text)
    while codes:
        m = monitor()
        while m.read(0xC6, 0xC6)[0] != 0:
            m.resume()
            time.sleep(0.2)
            m = monitor()
        chunk, codes = codes[:8], codes[8:]
        m.cmd('> 0277 ' + ' '.join('%02x' % c for c in chunk))
        m.cmd('> 00c6 %02x' % len(chunk))
        m.resume()
        time.sleep(0.1)


def main():
    op = sys.argv[1]
    if op == 'screen':
        m = monitor()
        print(screen(m))
        m.resume()
    elif op == 'type':
        type_text(sys.argv[2])
    elif op == 'wait':
        want = sys.argv[2]
        limit = time.time() + float(sys.argv[3] if len(sys.argv) > 3 else 30)
        while True:
            m = monitor()
            s = screen(m)
            m.resume()
            if want in s:
                print(s)
                return
            if time.time() > limit:
                print(s)
                print('TIMEOUT waiting for %r' % want)
                sys.exit(1)
            time.sleep(0.5)
    elif op == 'peek':
        m = monitor()
        a = int(sys.argv[2], 16)
        b = int(sys.argv[3], 16) if len(sys.argv) > 3 else a
        print(' '.join('%02x' % x for x in m.read(a, b)))
        m.resume()
    elif op == 'shot':
        m = monitor()
        chars = m.read(0x0800, 0x0FFF)
        scr = m.read(0x0400, 0x07E7)
        col = m.read(0xD800, 0xDBE7)
        bg = m.read(0xD021, 0xD021)[0]
        m.resume()
        write_png(sys.argv[2], render(chars, scr, col, bg))
    elif op == 'quit':
        m = monitor()
        m.s.sendall(b'quit\n')
    elif op == 'raw':
        m = monitor()
        print(m.cmd(sys.argv[2]))
        m.resume()


if __name__ == '__main__':
    main()
