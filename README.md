# SkinnyAI 64

A Commodore 64 port of [skinnyai](../skinnyai), the thin chat client for Ollama and OpenAI-compatible servers. It talks plain HTTP straight from the C64 through a SwiftLink-compatible ethernet modem, such as the TeensyROM's. You don't need a bridge program on another computer, just a server on your network.

```
 SkinnyAI 64                llama3.2:3b
>>> Is 91 prime? One sentence.
No, 91 is not prime because it is
divisible by 7 and 13.

>>>
```

## What you need

- A C64 (or C128 in 64 mode) with a **TeensyROM** set to *Special IO: Swift-Turbo/Modem*, with ethernet connected. Any SwiftLink at `$DE00` (NMI) whose modem understands the TeensyROM's `ATC` and `ATDT host:port` commands should work too.
- A disk drive (real, SD2IEC, Pi1541, ...) for settings and saved chats. Without one you can still chat; nothing is saved.
- A server that speaks **plain HTTP** (the C64 can't do TLS):
  - **Ollama** on your network. By default Ollama only listens on localhost, so start it with `OLLAMA_HOST=0.0.0.0 ollama serve` (or set that in its environment).
  - Or an **OpenAI-compatible** server: llama.cpp's `server`, LM Studio, vLLM, or Ollama's own `/v1` endpoints.

  ollama.com, OpenAI, and Anthropic are HTTPS-only, so they're out of reach unless you run an HTTP-to-HTTPS proxy on your network.

## Building

You need [cc65](https://cc65.github.io/) (`brew install cc65`). VICE's `c1541` is used to make the disk image.

```bash
make            # dist/skinnyai.prg and dist/skinnyai.d64
```

Copy `dist/skinnyai.prg` to the TeensyROM's SD card or USB stick and launch it from the TeensyROM menu, or use the disk image: `LOAD "SKINNYAI",8` then `RUN`.

## First run

With no `skinnyai.cfg` on the disk, it asks for the server's address, its port (11434 for Ollama), the API (`ollama` or `openai`), and a model, then saves them to `skinnyai.cfg`. Next it checks for the modem and runs `ATC` to start ethernet, showing the C64's IP address. Then type at the `>>>` prompt.

Change settings with `/set ...` and save them as the defaults with `/config`. The drive is the one the program was loaded from (`/set drive <n>` to change it).

## Commands

The command set follows skinnyai, which follows `ollama run`.

| Command | |
|---|---|
| `/set ...` | Change a setting (below) |
| `/show info` | Model details from the server (Ollama only) |
| `/show settings`, `system`, `parameters`, `modelfile` | This session's state; `modelfile` shows the chat as `/save` would write it |
| `/save [name]` | Save the chat to disk as `<name>.mf` (names up to 13 characters) |
| `/load <name>` | Load a saved chat; with no saved chat by that name, switch to that model with a new conversation (as `ollama run` does) |
| `/load` | List saved chats |
| `/new` | Start a new conversation (the system message stays) |
| `/delete [name]` | Delete a saved chat, after asking |
| `/list` | The server's models, then the saved chats |
| `/page` | The latest reply again, a screen at a time |
| `/model` | Model, host, and API |
| `/config` | Save the current server, model, key, and display settings to `skinnyai.cfg` |
| `/cls` | Clear the screen |
| `/bye`, `/exit` | Leave (offers to save first if the chat has unsaved messages). This resets the C64, which on a TeensyROM goes back to its menu |
| `/?`, `/help` | Help; `/? set`, `/? show`, `/? keys` |

`/set` takes `model <name>`, `host <address[:port]>`, `api ollama|openai`, `key <key>` (a bearer token, sent to any host), `system <text>`, `parameter <name> <value>` (for example `temperature 0.7`, `num_ctx 4096`, `stop <text>`), `format json` / `noformat`, `think [level]` / `nothink`, `showthinking` / `hidethinking`, `verbose` / `quiet` (token counts, speed, and bytes received), `markdown` / `nomarkdown`, `pager` / `nopager`, `autosave` / `noautosave`, `keepalive <time>` (Ollama, default `1h`), and `drive <8-30>`.

A message that starts with `//` is sent as a message starting with `/`.

## Keys

| Key | |
|---|---|
| RETURN | Send |
| SHIFT+RETURN | New line in the message |
| CRSR up / down | Earlier lines you've typed |
| CRSR left / right, DEL | Edit |
| HOME, CLR | Start of the line, clear it |
| RUN/STOP | Stop a reply (what arrived is kept) |
| SPACE / RETURN / Q at `-- more --` | Next page / one more line / stop paging |

The C64 has no `\ ^ _ { } | ~ `` ` `` keys, so the font is changed to provide them: **£** types `\`, **↑** types `^`, **←** types `_`, and SHIFT with **+ - @ \*** and **£** types `{ } ~ `` ` `` and `|`.

## Saved chats

`/save` writes a sequential file `<name>.mf` in ASCII with LF line endings. It is an Ollama Modelfile, the same format skinnyai's `/export` writes and its older versions saved:

```
# Saved by SkinnyAI 64 0.1
# api: ollama
# host: http://192.168.1.20:11434
# keep-alive: 1h
FROM llama3.2:3b
PARAMETER temperature 0.7
SYSTEM """You are a pirate."""
MESSAGE user """Greet me."""
MESSAGE assistant """Ahoy, matey!"""
```

So a chat can move between the C64 and skinnyai, and `ollama create pirate -f pirate.mf` turns it into a model. Copy the file off an SD2IEC card or a disk image (`c1541 disk.d64 -read "pirate.mf,s" pirate.mf`). `/load` reads Modelfiles from elsewhere too: `FROM`, `PARAMETER`, `SYSTEM`, `MESSAGE` with one-line or `"""` values, and the `# name: value` settings comments. Other instructions are skipped. If a loaded chat names a different server and you have an API key set, it asks before sending the key there.

## Limits and choices

Some things skinnyai does didn't make the trip:

- **No HTTPS**, so no ollama.com, OpenAI, or Anthropic directly, and no Anthropic API.
- **No tools** (web search, page fetching, MCP), no images or file attachments, no encrypted sessions or sync, no SQLite chat files, no profiles (just `skinnyai.cfg`), and no model-suggested titles. Chats are saved only when you `/save` (or with `/set autosave` once a chat has a name).
- **Memory:** the conversation lives in the 12 KB of RAM under the I/O area and the KERNAL ROM. The whole history is sent with each message, as in skinnyai. When it fills, the oldest exchanges are dropped, and you're told. A system message holds up to 1 KB and a typed message 600 characters. `/show settings` shows how much is used.
- **Speed:** the C64 handles roughly 2,300 bytes/s of response, measured in VICE. Ollama sends about 120 bytes of JSON per token, so replies appear at about 18 tokens a second. The modem holds the rest (it buffers 320 KB), so nothing is lost while the C64 catches up or the pager waits. OpenAI-style streams are similar.
- **Text:** replies are shown in ASCII. Accented letters lose their accents, curly quotes, dashes, arrows, ellipses, box drawing, and fractions become their ASCII look-alikes, and emoji are dropped. Markdown is shown as color: headings yellow, `**bold**` white, `code` and code blocks cyan, and thinking grey.
- **Thinking** (`/set think`) is shown as it streams, between "Thinking..." and "...done thinking.", and isn't kept in the history.
- `/show license` and `/show template` aren't available (too large), and `/show info` takes a while, since `/api/show` returns many kilobytes.

## How it works

- `src/acia.s`: the SwiftLink driver. Bytes arrive by NMI into a 256-byte ring at `$CF00`. At 3/4 full the handler raises RTS, so the TeensyROM holds further bytes until the program catches up. It also has `rx_pump`, the assembly loop that moves a response body through the JSON tokenizer, and the character set copy.
- `src/net.c`: the modem (`ATE0`, `ATV1`, `ATC`, `ATDT host:port`, `NO CARRIER`, `+++` with guard times) and HTTP/1.0: requests with `Content-Length` (the JSON body is generated twice, once to count it), and responses that are plain, `Content-Length`, or chunked.
- `src/json.c` + `src/jsonfast.s`: a streaming JSON tokenizer that reports values by key and streams `content` and `thinking` text a character at a time; UTF-8 and `\u` escapes become ASCII.
- `src/chat.c`: the history in far memory, request bodies for both APIs, the streamed reply, `/list`, `/show info`. `src/far.s` reaches the RAM under I/O and the KERNAL with both switched out; NMIs still arrive through a RAM vector at `$FFFA`.
- `src/screen.c` (direct screen writes, word wrap, markdown color, pager, keyboard mapping), `src/input.c` (the line editor), `src/disk.c` (Modelfiles, `skinnyai.cfg`, the directory, through the KERNAL), `src/main.c` (settings and commands).
- Memory: BASIC stub at `$0801`; RAM character set `$0800-$0FFF` (it overwrites the stub after it runs); program `$1000` to about `$B900`; heap and C stack to `$CF00`; receive ring `$CF00-$CFFF`; history `$D000-$FFF7`. Text is ASCII inside the program (`src/ascii.h` makes cc65 leave literals alone) and is converted only for the screen, the keyboard, and disk file names.

## Testing in VICE

No TeensyROM is needed to try it. `tools/fakemodem.js` stands in for the TeensyROM's modem. It answers the same AT commands the way the firmware does, opens real TCP connections, and says `NO CARRIER` when the server hangs up. VICE's emulated SwiftLink connects to it:

```bash
make vice       # builds, then runs tools/run-vice.sh
```

This starts the fake modem and x64sc with a SwiftLink at `$DE00` (NMI, 38400 baud) and a writable copy of the disk image in `test/out/work.d64`. At the setup prompt, give the address of an Ollama server (`127.0.0.1` reaches one on the same machine). VICE doesn't pass RTS flow control over TCP, so the fake modem paces its output instead (`FAKEMODEM_BPS`, default 2400 bytes/s; 3800 is the real line rate).

`tools/mon.py` drives the running emulator through VICE's remote monitor. It's how this was tested:

```bash
python3 tools/mon.py type 'Why is the sky blue?\r'
python3 tools/mon.py wait '>>>' 60
python3 tools/mon.py screen
python3 tools/mon.py shot test/out/screen.png
```

`test/chunked-server.js` serves a deliberately awkward chunked response, with chunk extensions, UTF-8, `\u` escapes, and emoji, for the HTTP and text paths. `make CFLAGS="-t c64 -Oirs --static-locals -DHIST_TOP=0xD400u"` builds with 1 KB of history, for testing what happens when memory fills.

Everything here was checked in VICE 3.10 against Ollama: both APIs, streaming, thinking, the pager, RUN/STOP, `/list`, `/show info`, save and load, errors, and full memory. It has **not yet been run on a real TeensyROM**. The modem behavior comes from reading the TeensyROM firmware source (`Swift_ATcommands.c`, `IOH_Swiftlink.c`), so that's the part most likely to need adjusting.

## License

Apache-2.0, like skinnyai.
