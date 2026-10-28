#!/usr/bin/env python3
"""End-to-end tests of SkinnyAI 64 in VICE.

Each test boots dist/skinnyai.d64 in x64sc with an emulated SwiftLink
wired to tools/fakemodem.js, which dials test/mock-server.js (a scripted
Ollama and OpenAI server that logs the requests it gets). The tests type
through VICE's remote monitor (tools/mon.py), read the screen, check the
requests, and look at the disk image afterwards.

  make test                       build, then run them all
  test/run.py [name ...]          just these (substrings of test names)

Environment: X64SC (the emulator, default x64sc), C1541, VICE_ARGS (more
options for x64sc, e.g. -minimized), TEST_TIMEOUT_SCALE (multiplies the
waits, for slow machines). Each test's files are in test/out/<test>/:
the disk image, the logs, and screen.txt and screen.png when it fails.
"""
import json
import os
import shlex
import shutil
import socket
import subprocess
import sys
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import mon  # noqa: E402

X64SC = os.environ.get('X64SC', 'x64sc')
C1541 = os.environ.get('C1541', 'c1541')
VICE_ARGS = shlex.split(os.environ.get('VICE_ARGS', ''))
SCALE = float(os.environ.get('TEST_TIMEOUT_SCALE', '1'))
OUT = os.path.join(ROOT, 'test', 'out')
MOCK_PORT, MODEM_PORT, MON_PORT = 11500, 25232, 6510
mon.ADDR = ('127.0.0.1', MON_PORT)

OLLAMA_CFG = {'host': '127.0.0.1', 'port': MOCK_PORT, 'api': 'ollama', 'model': 'mock:7b',
              'keepalive': '1h', 'pager': 0, 'markdown': 1}


class Failure(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Failure(msg)


def listening(port):
    try:
        socket.create_connection(('127.0.0.1', port), timeout=1).close()
        return True
    except OSError:
        return False


def wait_port(port, timeout=10, proc=None, log=None):
    limit = time.time() + timeout
    while time.time() < limit:
        if proc and proc.poll() is not None:
            tail = open(log, errors='replace').read()[-1500:] if log else ''
            raise Failure('it exited (%s) before listening on port %d\n%s' % (proc.returncode, port, tail))
        if listening(port):
            return
        time.sleep(0.1)
    raise Failure('nothing listening on port %d' % port)


def flat(text):
    """Text with its line breaks and runs of spaces made single spaces."""
    return ' '.join(text.split())


def at_prompt(rows):
    """Whether the last thing on screen is an empty "> " prompt."""
    lines = [r for r in rows if r.strip()]
    return bool(lines) and lines[-1] == '>'


def c1541(image, *args):
    return subprocess.run([C1541, image] + list(args), capture_output=True, text=True).stdout


class C64:
    """One emulator session, with its own disk image, fake modem and logs."""

    def __init__(self, name, cfg=None, modem=True, vice_args=(), modem_env=None, settle=0):
        self.settle = settle            # seconds to leave it alone before each look
        self.dir = os.path.join(OUT, name)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.d64 = os.path.join(self.dir, 'work.d64')
        shutil.copy(os.path.join(ROOT, 'dist', 'skinnyai.d64'), self.d64)
        if cfg is not None:
            path = os.path.join(self.dir, 'skinnyai.cfg')
            with open(path, 'w') as f:
                f.write(''.join('%s=%s\n' % kv for kv in cfg.items()))
            c1541(self.d64, '-write', path, 'skinnyai.cfg,s')
        self.reqlog = os.path.join(self.dir, 'requests.jsonl')
        self.procs = []
        for port in (MOCK_PORT, MODEM_PORT, MON_PORT):
            check(not listening(port), 'port %d is in use: is something left from another run?' % port)
        try:
            self.start(modem, vice_args, modem_env or {})
        except BaseException:
            self.quit()
            raise

    def start(self, modem, vice_args, modem_env):
        mon.disconnect()
        self.mock = self.spawn(['node', 'test/mock-server.js', str(MOCK_PORT), self.reqlog], 'mock.log')
        wait_port(MOCK_PORT)
        if modem:
            self.spawn(['node', 'tools/fakemodem.js', str(MODEM_PORT)], 'modem.log',
                       env=dict({'FAKEMODEM_ATC_DELAY': '200'}, **modem_env))
            wait_port(MODEM_PORT)
        vicerc = os.path.join(self.dir, 'vicerc')
        with open(vicerc, 'w') as f:
            f.write('[Version]\nConfigVersion=3.10\n')
        self.vice = self.spawn(
            [X64SC, '-config', vicerc, '-sounddev', 'dummy', '-autostart-warp']
            + (['-acia1', '-acia1base', '0xDE00', '-acia1irq', '1', '-acia1mode', '1', '-myaciadev', '0',
                '-rsdev1', '127.0.0.1:%d' % MODEM_PORT, '-rsdev1baud', '38400'] if modem else [])
            + ['-remotemonitor', '-remotemonitoraddress', 'ip4://127.0.0.1:%d' % MON_PORT,
             '-8', self.d64] + VICE_ARGS + list(vice_args) + ['-autostart', self.d64], 'vice.log')
        wait_port(MON_PORT, 30, self.vice, os.path.join(self.dir, 'vice.log'))

    def spawn(self, argv, log, env=None):
        p = subprocess.Popen(argv, cwd=ROOT, stdout=open(os.path.join(self.dir, log), 'w'),
                             stderr=subprocess.STDOUT, env=dict(os.environ, **(env or {})))
        self.procs.append(p)
        return p

    # ---- the screen and keyboard ----

    def rows(self):
        m = mon.monitor()
        try:
            return mon.screen(m).split('\n')
        finally:
            m.resume()

    def screen(self):
        return '\n'.join(self.rows())

    def type(self, text):
        mon.type_text(text)

    def wait(self, want, timeout=30, absent=False):
        """Waits until want (a string, or a function of the rows) is on screen."""
        test = want if callable(want) else (lambda rows: want in '\n'.join(rows))
        limit = time.time() + timeout * SCALE
        while True:
            time.sleep(self.settle)
            rows = self.rows()
            if test(rows):
                return rows
            if time.time() > limit:
                raise Failure('timed out waiting for %r' % (want if not callable(want) else want.__doc__))
            time.sleep(0.3)

    def run(self, line, expect=(), timeout=30, prompt=True):
        """Types a line and RETURN, then waits for the expected text below its
        echo and (unless prompt is false) a new "> " prompt; returns the text
        after the echo. Lines wrapped on the screen are joined with a space,
        so the expected text is found across a wrap."""
        if isinstance(expect, str):
            expect = [expect]
        expect = [flat(e) for e in expect]
        echo = ('> ' + line)[:40]
        self.type(line.replace('\\', '\\\\') + '\r')

        def after_echo(rows):
            for i in range(len(rows) - 1, -1, -1):
                if rows[i].startswith(echo):
                    return rows[i + 1:]
            return None

        def done(rows):
            after = after_echo(rows)
            if after is None:
                return False
            text = flat('\n'.join(after))
            return all(e in text for e in expect) and (not prompt or at_prompt(after))
        done.__doc__ = '%r then %r' % (line, expect)
        return flat('\n'.join(after_echo(self.wait(done, timeout))))

    def cls(self):
        self.type('/cls\r')

        def cleared(rows):
            """a clear screen"""
            return [r for r in rows[1:] if r.strip()] == ['>']
        self.wait(cleared)

    def ready(self, timeout=60):
        """Waits for the first prompt after the welcome."""
        def ready(rows):
            """the first prompt"""
            return 'Type /help' in '\n'.join(rows) and at_prompt(rows)
        return '\n'.join(self.wait(ready, timeout))

    def peek(self, addr):
        m = mon.monitor()
        try:
            return m.read(addr, addr)[0]
        finally:
            m.resume()

    # ---- the server's side ----

    def requests(self):
        if not os.path.exists(self.reqlog):
            return []
        with open(self.reqlog) as f:
            return [json.loads(line) for line in f]

    def last_request(self):
        reqs = self.requests()
        check(reqs, 'no request reached the server')
        return reqs[-1]

    # ---- the end ----

    def save_screen(self):
        try:
            with open(os.path.join(self.dir, 'screen.txt'), 'w') as f:
                f.write(self.screen() + '\n')
            m = mon.monitor()
            chars = m.read(0x0800, 0x0FFF)
            scr = m.read(0x0400, 0x07E7)
            col = m.read(0xD800, 0xDBE7)
            bg = m.read(0xD021, 0xD021)[0]
            m.resume()
            mon.write_png(os.path.join(self.dir, 'screen.png'), mon.render(chars, scr, col, bg))
        except Exception as e:  # the emulator may be gone
            print('    (no screen: %s)' % e)

    def quit(self):
        """Quits VICE, which writes the disk image back."""
        if getattr(self, 'vice', None) and self.vice.poll() is None:
            try:
                m = mon.monitor()
                m.s.sendall(b'quit\n')
            except OSError:
                pass
            try:
                self.vice.wait(10)
            except subprocess.TimeoutExpired:
                pass
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(5)
                except subprocess.TimeoutExpired:
                    p.kill()
        mon.disconnect()

    # ---- the disk image (after quit) ----

    def files(self):
        out = c1541(self.d64, '-list')
        names = []
        for line in out.splitlines():
            if '"' in line:
                names.append(line.split('"')[1])
        return names[1:]                # the first is the disk's name

    def read_file(self, name):
        dest = os.path.join(self.dir, 'read.tmp')
        if os.path.exists(dest):
            os.remove(dest)
        c1541(self.d64, '-read', name + ',s', dest)
        check(os.path.exists(dest), '%s is not on the disk (%s)' % (name, self.files()))
        with open(dest, 'rb') as f:
            return f.read().decode('latin1')


# ---- the tests ----

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


@test
def ollama_chat():
    """A conversation with Ollama: the request, the streamed reply, the
    history sent back, and the server's errors."""
    c = C64('ollama_chat', cfg=OLLAMA_CFG)
    yield c
    c.ready()
    out = c.run('Hello there', 'Reply 1 to Hello there.')
    req = c.last_request()
    check(req['method'] == 'POST' and req['path'] == '/api/chat', 'wrong endpoint: %s %s' % (req['method'], req['path']))
    check(req['version'] == 'HTTP/1.0', 'not HTTP/1.0')
    check(req['headers'].get('user-agent', '').startswith('SkinnyAI64/'), 'no User-Agent')
    check('authorization' not in req['headers'], 'an Authorization header with no key')
    body = req['body']
    check(body['model'] == 'mock:7b' and body['stream'] is True, 'model or stream: %r' % body)
    check(body['keep_alive'] == '1h', 'keep_alive: %r' % body.get('keep_alive'))
    check(body['messages'] == [{'role': 'user', 'content': 'Hello there'}], 'messages: %r' % body['messages'])
    check('tools' not in body, 'tools offered to a local model')

    # the whole conversation goes each time
    c.run('And again', 'Reply 3 to And again.')
    msgs = c.last_request()['body']['messages']
    check([m['role'] for m in msgs] == ['user', 'assistant', 'user'], 'roles: %r' % msgs)
    check(msgs[1]['content'] == 'Reply 1 to Hello there.', 'the reply kept: %r' % msgs[1]['content'])

    # a quoted, escaped message survives the trip
    c.run('Say "hi" \\ back', 'Reply 5 to Say "hi" \\ back.')
    check(c.last_request()['body']['messages'][-1]['content'] == 'Say "hi" \\ back', 'escaping')

    # settings that go into the request
    c.run('/set baud 115200', 'need a TeensyROM')
    c.run('/show settings', ['Serial: 38400', 'Receive: by NMI'])
    c.run('/set system You are terse.', 'Set system message.')
    c.run('/set parameter temperature 0.5', 'Set parameter.')
    c.run('/set parameter stop END', 'Set parameter.')
    c.run('/set keepalive 5m', 'Set keep-alive.')
    c.run('/set format json', 'Set JSON mode on.')
    c.cls()
    c.run('Settings now', 'Reply 8 to Settings now.')
    body = c.last_request()['body']
    check(body['messages'][0] == {'role': 'system', 'content': 'You are terse.'}, 'system: %r' % body['messages'][0])
    check(body.get('options') == {'temperature': 0.5, 'stop': ['END']}, 'options: %r' % body.get('options'))
    check(body['keep_alive'] == '5m', 'keep_alive: %r' % body['keep_alive'])
    check(body.get('format') == 'json', 'format: %r' % body.get('format'))
    c.run('/set noformat', 'Set JSON mode off.')

    # thinking, then token stats
    c.run('/set think', 'Set thinking on.')
    c.run('/set verbose', 'Set stats on.')
    c.cls()
    out = c.run('Please think', ['Thinking...', '...done thinking.', 'Reply 10 to Please think.', 'tokens/s'])
    check(c.last_request()['body'].get('think') is True, 'think: %r' % c.last_request()['body'].get('think'))
    check('total 1.5s, prompt 12 tokens, reply 7 tokens, 7.0 tokens/s' in out, 'stats line:\n' + out)
    c.run('/set quiet', 'Set stats off.')
    c.run('/set nothink', 'Set thinking off.')

    # a chunked response with UTF-8 and \u escapes
    c.cls()
    out = c.run('Send it chunked', 'done.')
    check('Caf' in out and 'quoted' in out and 'cup' in out, 'chunked reply:\n' + out)

    # errors: the question is taken back, so the history doesn't grow
    n = len(c.last_request()['body']['messages'])
    c.cls()
    out = c.run('nomodel please', 'Error: model "mock:7b" not found')
    check("(/list shows the server's models)" in out, 'no /list hint:\n' + out)
    out = c.run('hangup please', 'Error: The server closed the connection without answering.')
    c.run('Still there?', 'Still there.')
    check(len(c.last_request()['body']['messages']) == n + 2, 'failed turns were kept in the history')

    # CRSR up brings back earlier lines (from the buffer the start-up code ran in)
    c.cls()
    c.type('\\u\\u\\r')
    c.wait(lambda rows: 'to Still there.' in '\n'.join(rows) and at_prompt(rows))
    check(c.last_request()['body']['messages'][-1]['content'] == 'Still there?', 'recalled: %r' % c.last_request()['body']['messages'][-1])


@test
def commands():
    """/list, /show, /set model, help, the pager, markdown, unknown commands."""
    c = C64('commands', cfg=OLLAMA_CFG)
    yield c
    c.ready()
    out = c.run('/list', ['Models on the server:', 'mock:7b (7B)', 'tiny:1b (1B)', 'Saved chats:'])
    check(c.last_request()['path'] == '/api/tags', 'not /api/tags')
    out = c.run('/show info', ['family: mockfam', 'parameters: 7B', 'quantization: Q4_K_M',
                               'context length: 8192', 'capabilities: completion, tools'])
    check(c.last_request()['body'] == {'model': 'mock:7b'}, '/api/show body')
    c.cls()
    c.run('/show system', 'No system message was specified.')
    c.run('/show parameters', 'No parameters were specified.')
    c.run('/model', ['Model: mock:7b', 'Host: http://127.0.0.1:11500', 'API: ollama'])
    c.cls()
    c.run('/show settings', ['Model: mock:7b', 'Messages: 0', 'Kept in: 12K (no REU)', 'Pager: off',
                             'Serial: 38400', 'Modem: 10.0.0.64'])
    c.cls()
    c.run('/help', ['/set', '/bye'])
    c.cls()
    c.run('/? keys', 'RUN/STOP')
    c.run('/frob', "Unknown command '/frob'. Type /? for help.")

    # another model: the title bar and the request
    c.run('/set model tiny:1b', 'Switched model; the conversation continues.')
    check(c.rows()[0].endswith('tiny:1b'), 'title bar: %r' % c.rows()[0])
    c.run('Which model', 'Reply 1 to Which model.')
    check(c.last_request()['body']['model'] == 'tiny:1b', 'model not switched')

    # markdown: the heading's # and the ** go, the table gets lines
    c.cls()
    out = c.run('Some markdown', ['Heading', 'second item', 'alpha'])
    check('**' not in out and '# Heading' not in out, 'markdown left in:\n' + out)
    check('|Name' in out or '| Name' in out, 'no table:\n' + out)
    c.run('/set nomarkdown', 'Set markdown off.')
    c.cls()
    out = c.run('More markdown', ['**bold**'])

    # the pager stops a long reply at a full screen
    c.run('/set pager', 'Set pager on.')
    c.cls()
    c.type('Give me lines\r')
    rows = c.wait('-- more --')
    check('Line 40' not in '\n'.join(rows), 'the pager let it all through')
    c.type(' ')
    c.wait(lambda rows: 'Line 40' in '\n'.join(rows) and at_prompt(rows))
    c.type('/page\r')                     # clears the screen: no echo
    rows = c.wait('-- more --')
    check(rows[1] == 'Line 1', '/page starts at %r' % rows[1])
    c.type('q')
    c.wait(lambda rows: at_prompt(rows))


@test
def save_and_load():
    """/save, /load, /delete, /config, and the files on the disk."""
    c = C64('save_and_load', cfg=OLLAMA_CFG)
    yield c
    c.ready()
    c.run('/set system Be brief.', 'Set system message.')
    c.run('/set parameter top_k 20', 'Set parameter.')
    c.run('First question', 'Reply 2 to First question.')
    c.run('/save chat1', "Saved 'chat1' (chat1.mf). Resume it with /load chat1")
    c.run('/new', 'Started a new conversation')
    c.run('/show settings', ['Messages: 0', 'Session: (not saved)'])
    c.cls()
    c.run('/load chat1', ["Loaded 'chat1': mock:7b, 2 messages.", 'First question', 'Reply 2 to First question.'])
    c.run('Second question', 'Reply 4 to Second question.')
    msgs = c.last_request()['body']['messages']
    check([m['role'] for m in msgs] == ['system', 'user', 'assistant', 'user'], 'loaded history: %r' % msgs)
    c.run('/save', "Saved 'chat1'")
    c.cls()
    c.run('/load', ['Saved chats:', 'chat1'])
    c.run('/save chat2', "Saved 'chat2'")
    c.run('/delete chat2', 'Delete it? (y/N)', prompt=False)
    c.type('y')
    c.wait('Deleted.')
    c.run('/load nosuch:1b', "No saved chat by that name; switched to model 'nosuch:1b' with a new conversation.")
    c.run('/set pager', 'Set pager on.')
    c.run('/config', 'Saved these settings to skinnyai.cfg.')
    c.run('/bye', prompt=False)
    c.wait('basic bytes free', 30)         # /bye resets the C64
    c.quit()

    files = c.files()
    check('chat1.mf' in files and 'chat2.mf' not in files, 'files on disk: %r' % files)
    mf = c.read_file('chat1.mf')
    for want in ('FROM mock:7b\n', 'PARAMETER top_k 20\n', 'SYSTEM """Be brief."""\n',
                 'MESSAGE user """First question"""\n', 'MESSAGE assistant """Reply 4 to Second question."""\n'):
        check(want in mf, 'chat1.mf has no %r:\n%s' % (want, mf))
    cfg = c.read_file('skinnyai.cfg')
    for want in ('host=127.0.0.1\n', 'port=11500\n', 'api=ollama\n', 'model=nosuch:1b\n', 'pager=1\n'):
        check(want in cfg, 'skinnyai.cfg has no %r:\n%s' % (want, cfg))


@test
def cloud_tools():
    """A -cloud model gets web_search and fetch_page, which run through the
    server's /api/experimental endpoints, and the calls and results go back
    in the history; saved chats leave them out."""
    c = C64('cloud_tools', cfg=dict(OLLAMA_CFG, model='mock:7b-cloud'))
    yield c
    c.ready()
    c.run('/show settings', 'Tools: web_search, fetch_page')
    c.cls()
    out = c.run('Please search', ['Looking.', 'Searching: c64 news',
                                  'Fetching: https://example.com/a/', 'Read 6: C64 news/The C64/is'])
    reqs = c.requests()
    check([r['path'] for r in reqs] == ['/api/chat', '/api/experimental/web_search', '/api/chat',
                                        '/api/experimental/web_fetch', '/api/chat'],
          'requests: %r' % [r['path'] for r in reqs])
    tools = reqs[0]['body'].get('tools') or []
    check([t['function']['name'] for t in tools] == ['web_search', 'fetch_page'], 'tools: %r' % tools)
    check('Today is Mon, 26 Oct 2026.' not in tools[0]['function']['description'], 'a date before any reply')
    check(reqs[1]['body'] == {'query': 'c64 news', 'max_results': 3}, 'search: %r' % reqs[1]['body'])
    check(reqs[3]['body'] == {'url': 'https://example.com/a/rather/long/path/to/a/page/about/the/commodore/64/news.html'},
          'fetch: %r' % reqs[3]['body'])
    check('Today is Mon, 26 Oct 2026.' in reqs[2]['body']['tools'][0]['function']['description'], 'no date from the server')
    msgs = reqs[4]['body']['messages']
    check([m['role'] for m in msgs] == ['user', 'assistant', 'assistant', 'tool', 'assistant', 'tool'],
          'roles: %r' % msgs)
    check(msgs[1] == {'role': 'assistant', 'content': 'Looking.'}, 'content before the call: %r' % msgs[1])
    check(msgs[2] == {'role': 'assistant', 'content': '', 'tool_calls': [
        {'function': {'name': 'web_search', 'arguments': {'query': 'c64 news'}}}]}, 'the call: %r' % msgs[2])
    result = msgs[3]['content']
    check(msgs[3]['tool_name'] == 'web_search', 'tool_name: %r' % msgs[3])
    check(result.startswith('Result 1\nhttps://example.com/1\nText of\nresult 1. yyy'), 'search result: %r' % result[:80])
    check('\n\nResult 3\n' in result and 'Result 4' not in result and len(result) < 2400, 'search result:\n' + result)
    check(msgs[4]['tool_calls'][0]['function']['name'] == 'fetch_page', 'fetch call: %r' % msgs[4])
    check(msgs[5]['tool_name'] == 'fetch_page' and msgs[5]['content'].startswith('C64 news\nThe C64\nis back. xxx'),
          'fetch result: %r' % msgs[5]['content'][:60])
    check(1500 < len(msgs[5]['content']) < 1650, 'fetch result length %d' % len(msgs[5]['content']))

    # the tool messages stay in the conversation
    c.cls()
    c.run('And then?', 'Reply 8 to And then.')
    check(len(c.last_request()['body']['messages']) == 8, 'history: %r' % c.last_request()['body']['messages'])

    # a tool's error goes to the model too
    c.cls()
    out = c.run('Search and fail', ['Searching: fail please', 'unauthorized', 'Read 13: Error: unauthorized'])
    check(c.last_request()['body']['messages'][-1]['content'] == 'Error: unauthorized', 'error result')

    # at most 5 rounds of tools, then an answer without them
    c.cls()
    n = len(c.requests())
    c.run('Go on forever', 'Read 25: Result 1/https://exa', timeout=90)
    chats = [r for r in c.requests()[n:] if r['path'] == '/api/chat']
    check(len(chats) == 6 and all('tools' in r['body'] for r in chats[:5]) and 'tools' not in chats[5]['body'],
          'rounds: %r' % [('tools' in r['body']) for r in chats])

    # a model without -cloud gets no tools, but the history still goes
    c.run('/set model mock:7b', 'Switched model')
    c.run('Local now', 'Reply 27 to Local now.')
    check('tools' not in c.last_request()['body'], 'tools for a local model')
    c.run('/show settings', 'Tools: off')
    c.run('/save tooled', 'Saved')
    c.quit()
    mf = c.read_file('tooled.mf')
    check(mf.count('MESSAGE user') == 5 and mf.count('MESSAGE assistant') == 7 and 'Text of' not in mf,
          'saved chat:\n' + mf)


@test
def openai_api():
    """The OpenAI-compatible API: the path, the key, SSE, reasoning, /list."""
    c = C64('openai_api', cfg={'host': '127.0.0.1', 'port': MOCK_PORT, 'api': 'openai', 'model': 'gpt-mock',
                               'key': 'sk-test-123', 'pager': 0})
    yield c
    out = c.ready()
    check('OpenAI-compatible' in out, 'welcome:\n' + out)
    c.run('Hello OpenAI', 'Reply 1 to Hello OpenAI.')
    req = c.last_request()
    check(req['path'] == '/v1/chat/completions', 'path: %s' % req['path'])
    check(req['headers'].get('authorization') == 'Bearer sk-test-123', 'auth: %r' % req['headers'].get('authorization'))
    check('keep_alive' not in req['body'] and 'options' not in req['body'], 'Ollama fields: %r' % req['body'])
    c.run('/set parameter num_predict 50', 'Set parameter.')
    c.run('/set think high', 'Set thinking on.')
    c.run('/set verbose', 'Set stats on.')
    c.cls()
    c.run('Think it over', ['Thinking...', 'Reply 3 to Think it over.'])
    body = c.last_request()['body']
    check(body.get('max_tokens') == 50, 'max_tokens: %r' % body)
    check(body.get('reasoning_effort') == 'high', 'reasoning_effort: %r' % body)
    check(body.get('stream_options') == {'include_usage': True}, 'stream_options: %r' % body)
    c.cls()
    c.run('/list', ['gpt-mock', 'gpt-tiny'])
    check(c.last_request()['path'] == '/v1/models', 'not /v1/models')
    c.run('/show info', '/show info needs the Ollama API.')


@test
def first_run():
    """With no skinnyai.cfg it asks for the server and saves the answers."""
    c = C64('first_run', cfg=None)
    yield c
    c.wait('Server address:', 60)
    c.type('http://127.0.0.1\r')
    c.wait('Port:')
    c.type('\\c11500\r')                  # \c clears the 11434 offered
    c.wait('API (ollama or openai):')
    c.type('\r')
    out = '\n'.join(c.wait('Model:', 60))   # after the modem starts and the models are listed
    check('Models on the server:' in out and 'tiny:1b (1B)' in out, 'setup:\n' + out)
    c.type('mock:7b\r')
    out = c.ready()
    check('http://127.0.0.1:11500' in out and 'mock:7b' in out, 'welcome:\n' + out)
    c.run('Hi', 'Reply 1 to Hi.')
    c.quit()
    cfg = c.read_file('skinnyai.cfg')
    check('host=127.0.0.1\n' in cfg and 'port=11500\n' in cfg and 'model=mock:7b\n' in cfg, 'skinnyai.cfg:\n' + cfg)


@test
def reu_history():
    """With an REU the history goes there, and /set context sizes it."""
    c = C64('reu_history', cfg=OLLAMA_CFG, vice_args=['-reu', '-reusize', '512'])
    yield c
    out = c.ready()
    check('REU, 32K of 512K' in out, 'welcome:\n' + out)
    c.run('In the REU', 'Reply 1 to In the REU.')
    c.run('Still in the REU', 'Reply 3 to Still in the REU.')
    check(len(c.last_request()['body']['messages']) == 3, 'history')
    c.run('/set context 8', 'Set.')
    c.run('/show settings', ['Kept in: REU, 8K of 512K', 'Messages: 4'])


@test
def teensyrom_poll():
    """A modem whose ATI says TeensyROM is polled, and takes the Turbo232
    rates. A TeensyROM waits for each byte to be read before sending the
    next; VICE's ACIA doesn't, and it runs a frame's worth of cycles at a
    time, so bytes that reach it during a frame's wait arrive back to back,
    faster than polling reads them. So the fake modem sends one byte per
    40 ms (two frames), and the screen is only looked at (which pauses the
    emulator, and what queues up meanwhile then arrives at once) when no
    bytes are on their way."""
    c = C64('teensyrom_poll', cfg=dict(OLLAMA_CFG, baud=230400), vice_args=['-acia1mode', '2'], settle=20,
            modem_env={'FAKEMODEM_ATI': 'TeensyROM+ v0.9.0.28', 'FAKEMODEM_BPS': '25', 'FAKEMODEM_SPACED': '1'})
    yield c
    out = c.ready(120)
    check('needs a TeensyROM' not in out and "network didn't start" not in out, 'welcome:\n' + out)
    c.settle = 0.5
    c.run('/show settings', ['Serial: 230400', 'Receive: polled (TeensyROM)'])
    c.cls()
    c.settle = 50                       # a reply takes about 35 s
    c.run('Polled', 'Reply 1 to Polled.', timeout=150)
    c.settle = 0.5
    c.run('/set baud 115200', 'Serial rate: 115200 baud')
    c.cls()
    c.settle = 50
    c.run('Still polled', 'Reply 3 to Still polled.', timeout=150)


@test
def no_modem():
    """With no SwiftLink cartridge it says so, and messages fail with a reason."""
    c = C64('no_modem', cfg=OLLAMA_CFG, modem=False)
    yield c
    out = c.ready(90)
    check('No modem answered on the SwiftLink' in out, 'welcome:\n' + out)
    c.run('Anyone?', 'Error: No modem')
    c.run('/show settings', 'Modem: not found')
    check(not c.requests(), 'a request got through')


def main():
    only = sys.argv[1:]
    tests = [t for t in TESTS if not only or any(o in t.__name__ for o in only)]
    if not os.path.exists(os.path.join(ROOT, 'dist', 'skinnyai.d64')):
        sys.exit('dist/skinnyai.d64 is missing: run make first')
    failed = []
    t_all = time.time()
    for t in tests:
        t0 = time.time()
        print('%-20s ' % t.__name__, end='', flush=True)
        gen = t()
        c = None
        try:
            c = next(gen)
            for _ in gen:
                pass
            print('ok    %5.1fs' % (time.time() - t0))
        except Exception as e:
            failed.append(t.__name__)
            print('FAIL  %5.1fs' % (time.time() - t0))
            print('    %s' % (e if isinstance(e, Failure) else traceback.format_exc().rstrip().replace('\n', '\n    ')))
            if c and c.vice.poll() is None:
                c.save_screen()
                print('    screen (test/out/%s/screen.txt):' % t.__name__)
                for row in open(os.path.join(c.dir, 'screen.txt')).read().rstrip().split('\n'):
                    print('    | ' + row)
        finally:
            if c:
                c.quit()
    print('%d passed, %d failed in %.0fs' % (len(tests) - len(failed), len(failed), time.time() - t_all))
    if failed:
        print('failed: ' + ' '.join(failed))
        sys.exit(1)


if __name__ == '__main__':
    main()
