# SkinnyAI 64

[![Tests](https://github.com/wesbiggs/skinnyai-c64/actions/workflows/test.yml/badge.svg)](https://github.com/wesbiggs/skinnyai-c64/actions/workflows/test.yml)

A Commodore 64 port of [skinnyai](https://github.com/wesbiggs/skinnyai), the thin chat client for Ollama and OpenAI-compatible servers. It talks plain HTTP straight from the C64 through a SwiftLink-compatible ethernet modem, such as the TeensyROM's. You don't need a bridge program on another computer, just a server on your network.

```
 SkinnyAI                   llama3.2:3b
> Is 91 prime? One sentence.
No, 91 is not prime because it is
divisible by 7 and 13.

>
```

The top line is a green bar with the skinnyai logo (a thin white SKINNY and a bold light green AI, drawn as custom characters) and the model; you type after a yellow `>` in light green.

## What you need

- A C64 (or C128 in 64 mode) with a **SwiftLink-compatible** cartridge at `$DE00` (NMI) whose modem connects to the network with `ATDT"host:port"`. The **TeensyROM** is the one this was written for: set *Special IO* to *Swift-Turbo/Modem* and connect ethernet. Its `ATC` command starts ethernet, and a modem that doesn't know `ATC` is assumed to be online already, so a WiFi modem along the lines of Zimodem should work too (untested).
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

With no `skinnyai.cfg` on the disk, it asks for the server's address, its port (11434 for Ollama), the API (`ollama` or `openai`), and a model, then saves them to `skinnyai.cfg`. Then it shows the settings in a table, quietly checks for the modem (and asks it with `ATI` whether it's a TeensyROM) and runs `ATC` to start ethernet (only problems are reported; `/show settings` has the C64's IP address), and you type at the `>` prompt.

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

`/set` takes `model <name>`, `host <address[:port]>`, `api ollama|openai`, `key <key>` (a bearer token, sent to any host), `system <text>`, `parameter <name> <value>` (for example `temperature 0.7`, `num_ctx 4096`, `stop <text>`), `format json` / `noformat`, `think [level]` / `nothink`, `showthinking` / `hidethinking`, `verbose` / `quiet` (token counts, speed, and bytes received), `markdown` / `nomarkdown`, `pager` / `nopager`, `autosave` / `noautosave`, `keepalive <time>` (Ollama, default `1h`), `context <4-63>` (K of REU for the history), `baud 38400|57600|115200|230400` (the serial rate; above 38400 on a TeensyROM only, see below), and `drive <8-30>`.

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

## Web search with cloud models

Ollama's cloud models (`gpt-oss:120b-cloud`, `gemma4:31b-cloud`, ...) run through your local Ollama once it's signed in to ollama.com (`ollama signin`), and that Ollama also passes on Ollama's web search and page fetching over plain HTTP. So with `api ollama` and a model whose name ends in `-cloud`, the model is offered skinnyai's two tools:

- `web_search` returns the top 3 results' titles, URLs, and the first 400 characters of each page's text.
- `fetch_page` returns the first 1,500 characters of a page's text.

When the model calls one, a grey `Searching: ...` or `Fetching: ...` line shows, the C64 makes the request to the server's `/api/experimental/web_search` or `web_fetch`, and the result goes back to the model, which answers or calls another (up to 5 rounds; then it has to answer). A failed call (say, a usage limit) is shown in red and passed to the model too. The calls and results stay in the history, so follow-ups can refer to them, and use memory like any message; `/save` leaves them out, since a Modelfile has no place for them. Searches count against your ollama.com account's limits. The model is told the date from the server's last reply, since the C64 has no clock. `/show settings` says whether the tools are on.

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
- **Tools only for Ollama cloud models** (above), and no MCP, no images or file attachments, no encrypted sessions or sync, no SQLite chat files, no profiles (just `skinnyai.cfg`), and no model-suggested titles. Chats are saved only when you `/save` (or with `/set autosave` once a chat has a name).
- **Memory:** the conversation lives in the 12 KB of RAM under the I/O area and the KERNAL ROM, or, with an **REU** (RAM Expansion Unit), in up to 63 KB of it: 32 KB by default, changed with `/set context <4-63>` and saved by `/config`. The welcome table says which (`History: REU, 32K of 512K`). The whole history is sent with each message, as in skinnyai. When it fills, the oldest exchanges are dropped, and you're told. A system message holds up to 1 KB and a typed message 600 characters. `/show settings` shows how much is used.
  - More context costs time on each turn: at 38400 baud, 32 KB takes about 9 seconds to send, before the model starts, and the server keeps only its own context's worth (Ollama's `num_ctx`, which you may need to raise: `/set parameter num_ctx 16384`).
  - On the TeensyROM+ the REU is one of the *Special IO* choices, like the modem, and only one can be active, so the REU isn't available alongside the network there. A separate REU (1764, 1750, an Ultimate's) works.
- **Speed:** the C64 handles roughly 2,300 bytes/s of response, measured in VICE. Ollama sends about 120 bytes of JSON per token, so replies appear at about 18 tokens a second. The modem holds the rest (it buffers 320 KB), so nothing is lost while the C64 catches up or the pager waits. OpenAI-style streams are similar.
- **Serial rate:** 38400 baud, a SwiftLink's top speed, by default. When the modem answers `ATI` with `TeensyROM...`, received bytes are polled rather than taken by NMI: the TeensyROM sends the next byte only once the last has been read, so nothing is lost, and the hundred or so cycles an NMI costs each byte are saved. Then the Turbo232's faster rates are available too, `/set baud 57600|115200|230400` (and `/config` saves the choice). They only shorten the least gap the TeensyROM leaves between bytes, though, and the C64's parsing is the slower part, so expect little difference; sending isn't affected at all. Any other SwiftLink is a real 6551, which receives at the line rate whether or not the last byte has been read, so it needs the NMI and stays at 38400; a saved faster rate drops back to 38400 at startup, with a message. (By NMI, the TeensyROM's gap at 115200 and 230400, 68 and 34 cycles, is less than the handler takes, so the NMIs would pile up until the stack overflowed.)
- **Text:** replies are shown in ASCII. Accented letters lose their accents, curly quotes, dashes, arrows, ellipses, box drawing, and fractions become their ASCII look-alikes, and emoji are dropped. Markdown is shown as color: headings yellow, `**bold**` white, `code` and code blocks cyan, and thinking grey. Tables are drawn with the C64's line-drawing characters in green, with the header row in white. Each column is as wide as it needs to be, narrowed to fit 40 columns, and cells wrap onto more lines. A table holds up to 1 KB of text and 6 columns.
- **Thinking** (`/set think`) is shown as it streams, between "Thinking..." and "...done thinking.", and isn't kept in the history.
- `/show license` and `/show template` aren't available (too large), and `/show info` takes a while, since `/api/show` returns many kilobytes.

## How it works

- `src/acia.s`: the SwiftLink driver. Bytes arrive by NMI into a 256-byte ring at `$CF00`. At 3/4 full the handler raises RTS, so the TeensyROM holds further bytes until the program catches up. On a TeensyROM the receive interrupt is off and the chip is polled instead. It also has `rx_pump`, the assembly loop that moves a response body through the JSON tokenizer, and the character set copy.
- `src/net.c`: the modem (`ATE0`, `ATV1`, `ATI`, `ATC`, `ATDT"host:port"`, `NO CARRIER`, `+++` with guard times) and HTTP/1.0: requests with `Content-Length` (the JSON body is generated twice, once to count it), and responses that are plain, `Content-Length`, or chunked.
- `src/json.c` + `src/jsonfast.s`: a streaming JSON tokenizer that reports values by key and streams `content` and `thinking` text a character at a time; UTF-8 and `\u` escapes become ASCII.
- `src/chat.c`: the history, request bodies for both APIs, the streamed reply, the tools, `/list`, `/show info`. Each message has a header with its length and its length as JSON, so going through the history and working out `Content-Length` never reads the text. `src/far.s` holds the history: in an REU (found at startup by its registers keeping what's written, sized by marking each 64K bank), reached by DMA through a 128-byte cache line in the cassette buffer, or in the RAM under I/O and the KERNAL with both switched out, where NMIs still arrive through a RAM vector at `$FFFA`.
- The title bar's green is seven double-width sprites behind the first text row, so its characters can be white; the logo is seven custom characters. The sprites are turned off while the disk is in use, since their DMA cycles can upset the KERNAL's serial bus timing.
- `src/screen.c` (direct screen writes, word wrap, markdown color and tables, pager, keyboard mapping), `src/input.c` (the line editor), `src/disk.c` (Modelfiles, `skinnyai.cfg`, the directory, through the KERNAL), `src/main.c` (settings and commands).
- Memory: BASIC stub at `$0801`; RAM character set `$0800-$0FFF` (it overwrites the stub after it runs); program and its variables `$1000` to about `$C910`; the line editor's recall buffer `$CA80-$CDFF`, where the start-up code (main.c's first-run questions and welcome) runs before anything is typed (`src/boot.s` copies it there); C stack `$CE00-$CEFF` (with static locals, it holds only arguments: the tests never use more than 32 bytes); receive ring `$CF00-$CFFF`; title bar sprite data `$0340` and the REU cache line `$0380` (the cassette buffer); history `$D000-$FFF7`. Text is ASCII inside the program (`src/ascii.h` makes cc65 leave literals alone) and is converted only for the screen, the keyboard, and disk file names.

## Testing in VICE

No TeensyROM is needed to try it. `tools/fakemodem.js` stands in for the TeensyROM's modem. It answers the same AT commands the way the firmware does, opens real TCP connections, and says `NO CARRIER` when the server hangs up. VICE's emulated SwiftLink connects to it:

```bash
make vice       # builds, then runs tools/run-vice.sh
```

This starts the fake modem and x64sc with a SwiftLink at `$DE00` (NMI, 38400 baud) and a writable copy of the disk image in `test/out/work.d64`. At the setup prompt, give the address of an Ollama server (`127.0.0.1` reaches one on the same machine). VICE doesn't pass RTS flow control over TCP, so the fake modem paces its output instead (`FAKEMODEM_BPS`, default 2400 bytes/s; 3800 is the real line rate) and answers `ATC` after `FAKEMODEM_ATC_DELAY` ms (default 2000), as DHCP might. It answers `ATI` with a name that isn't a TeensyROM's, unless `FAKEMODEM_ATI` gives one (`FAKEMODEM_ATI='TeensyROM+ v0.9' FAKEMODEM_BPS=25 FAKEMODEM_SPACED=1` to try polling: VICE's ACIA doesn't wait for a byte to be read, and bytes that arrive while it waits between frames come in back to back, so polling needs no more than one a frame).

`tools/mon.py` drives the running emulator through VICE's remote monitor. It's how this was tested:

```bash
python3 tools/mon.py type 'Why is the sky blue?\r'
python3 tools/mon.py wait '> ' 60
python3 tools/mon.py screen
python3 tools/mon.py shot test/out/screen.png
```

### Automated tests

`make test` runs the tests in [`test/run.py`](test/run.py), which need VICE (`x64sc`, `c1541`), Node, and Python 3. Each test boots the disk image in x64sc with the fake modem, which dials `test/mock-server.js`, a scripted Ollama and OpenAI-compatible server that logs every request. The tests type through the remote monitor, read the screen, check the requests' JSON, and afterwards read the saved files off the disk image. They cover first-run setup, chatting over both APIs (the history sent back, system messages, parameters, thinking, stats, a chunked UTF-8 reply, server errors), web search and page fetching with a cloud model, `/list`, `/show`, markdown, the pager, saving and loading chats, `/config`, an REU, a TeensyROM's polled receive and faster rates, and a C64 with no SwiftLink. `test/run.py openai` runs only the tests whose names contain `openai`. A failed test leaves its screen (`screen.txt` and `screen.png`), logs, and disk image in `test/out/<test>/`. Set `VICE_ARGS=-minimized` to keep the emulator windows out of the way.

GitHub Actions runs them on every push and pull request ([`.github/workflows/test.yml`](.github/workflows/test.yml)), with VICE built from source with its headless UI.

### By hand

`test/chunked-server.js` serves a deliberately awkward chunked response, with chunk extensions, UTF-8, `\u` escapes, and emoji, for the HTTP and text paths. `make CFLAGS="-t c64 -Ors --static-locals -DHIST_TOP=0xD400u"` builds with 1 KB of history, for testing what happens when memory fills. For an REU, add VICE's options: `tools/run-vice.sh -reu -reusize 512`. For the faster rates, `-acia1mode 2` (VICE's Turbo232), with the fake modem saying it's a TeensyROM (above).

Everything here was checked in VICE 3.10 against Ollama: both APIs, streaming, thinking, the pager, RUN/STOP, `/list`, `/show info`, save and load, errors, full memory, polling at 230400 baud through VICE's Turbo232, and VICE's emulated 512 KB REU (a 19 KB chat loaded, sent, and remembered; `/set context` dropping old turns). It has **not yet been run on a real TeensyROM**. The modem behavior comes from reading the TeensyROM firmware source (`Swift_ATcommands.c`, `IOH_Swiftlink.c`), so that's the part most likely to need adjusting.

## License

[Apache-2.0](LICENSE), like skinnyai.
