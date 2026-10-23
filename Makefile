# Builds dist/skinnyai.prg (and a disk image) with cc65.
CL65   ?= cl65
C1541  ?= c1541
CFLAGS  = -t c64 -Ors --static-locals -g
SRC     = src/main.c src/screen.c src/input.c src/json.c src/net.c src/chat.c src/disk.c
ASM     = src/acia.s src/far.s src/jsonfast.s src/exehdr.s
DIST    = dist

all: $(DIST)/skinnyai.prg $(DIST)/skinnyai.d64

$(DIST)/skinnyai.prg: $(SRC) $(ASM) src/skinny.h src/ascii.h src/skinny.cfg
	mkdir -p $(DIST)
	$(CL65) $(CFLAGS) -C src/skinny.cfg -m $(DIST)/skinnyai.map -Ln $(DIST)/skinnyai.lbl -o $@ $(SRC) $(ASM)

$(DIST)/skinnyai.d64: $(DIST)/skinnyai.prg
	rm -f $@
	$(C1541) -format "skinnyai,64" d64 $@ -write $(DIST)/skinnyai.prg skinnyai >/dev/null

# Runs it in VICE with the fake TeensyROM modem (see tools/run-vice.sh).
vice: all
	tools/run-vice.sh

# Runs the tests in VICE (see test/run.py).
test: all
	test/run.py

clean:
	rm -rf $(DIST) src/*.o

.PHONY: all vice test clean
