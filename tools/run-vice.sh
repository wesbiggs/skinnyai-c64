#!/bin/sh
# Runs SkinnyAI 64 in VICE with an emulated SwiftLink at $DE00 (NMI) wired
# to tools/fakemodem.js, which stands in for the TeensyROM's ethernet modem.
# A writable copy of the disk image is used, so /save and /config work.
#
#   tools/run-vice.sh            fake modem on 25232, remote monitor on 6510
#
# Then drive it with tools/mon.py (see that file), or just type in the window.
set -e
cd "$(dirname "$0")/.."
mkdir -p test/out
# keep the saved files, refresh the program
[ -f test/out/work.d64 ] || cp dist/skinnyai.d64 test/out/work.d64
c1541 test/out/work.d64 -delete skinnyai -write dist/skinnyai.prg skinnyai >/dev/null 2>&1
node tools/fakemodem.js 25232 > test/out/modem.log 2>&1 &
MODEM=$!
trap 'kill $MODEM 2>/dev/null' EXIT INT TERM
sleep 0.5
# A config file of our own, so the user's VICE settings don't apply; it
# needs the version tag or VICE warns about it at startup.
VICE_VERSION=$(c1541 -version 2>/dev/null | sed -n 's/.*(VICE \([0-9.]*\)).*/\1/p')
printf '[Version]\nConfigVersion=%s\n' "${VICE_VERSION:-3.10}" > test/out/vicerc
x64sc -config test/out/vicerc -sounddev dummy \
  -acia1 -acia1base 0xDE00 -acia1irq 1 -acia1mode 1 -myaciadev 0 \
  -rsdev1 127.0.0.1:25232 -rsdev1baud 38400 \
  -remotemonitor -remotemonitoraddress ip4://127.0.0.1:6510 \
  -8 test/out/work.d64 -autostart test/out/work.d64 "$@" > test/out/vice.log 2>&1
