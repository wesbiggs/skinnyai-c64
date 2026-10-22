/* Screen output: a 40x24 scrolling area under a one-line status bar, written
 * straight to screen memory. Text is ASCII and is word-wrapped as it
 * streams; md_c adds a little markdown (headings, **bold**, `code`, code
 * blocks) as color. A pager stops at each full screen. */
#include "ascii.h"
#include <string.h>
#include <stdlib.h>
#include <cbm.h>
#include "skinny.h"

unsigned char crow = 1, ccol, curcolor = C_TEXT;
unsigned char a2s[128];
unsigned char pg_on, pg_lines, pg_quit;
unsigned char md_on = 1;

static unsigned char soft;              /* cursor is at a line start from wrapping */
static unsigned char wbuf[40], wclr[40], wlen;

/* Glyphs for ASCII characters the C64 doesn't have, put into the RAM
 * character set in place of PETSCII's pound sign, arrows, and a few
 * block graphics. */
static const unsigned char glyphs[] = {
    0x1C, 0x00, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x00,   /* \ */
    0x1E, 0x18, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00,   /* ^ */
    0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,   /* _ */
    0x7A, 0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,   /* ` */
    0x7B, 0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0x00,   /* { */
    0x7C, 0x70, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x70, 0x00,   /* } */
    0x7E, 0x00, 0x00, 0x76, 0xDC, 0x00, 0x00, 0x00, 0x00,   /* ~ */
};

static unsigned char to_screen(unsigned char c)
{
    if (c >= 0x61 && c <= 0x7A) return c - 0x60;          /* a-z */
    if (c == 0x40) return 0;                                /* @ */
    if (c >= 0x41 && c <= 0x5A) return c;                   /* A-Z */
    if (c >= 0x5B && c <= 0x5F) return c - 0x40;           /* [ \ ] ^ _ */
    if (c >= 0x20 && c <= 0x3F) return c;
    switch (c) {
    case '`': return 0x7A;
    case '{': return 0x7B;
    case '|': return 0x5D;                                  /* box line */
    case '}': return 0x7C;
    case '~': return 0x7E;
    }
    return '?';
}

void scr_init(void)
{
    unsigned char i, j;
    const unsigned char *g;
    unsigned char *p;

    charset_init();
    for (g = glyphs; g < glyphs + sizeof glyphs; g += 9) {
        p = (unsigned char *)0x0800 + g[0] * 8;
        for (j = 0; j < 8; ++j) {
            p[j] = g[j + 1];
            p[j + 1024] = g[j + 1] ^ 0xFF;                  /* reverse video */
        }
    }
    for (i = 0; i < 128; ++i) a2s[i] = to_screen(i);
    *(unsigned char *)0xD020 = C_BLACK;
    *(unsigned char *)0xD021 = C_BLACK;
    scr_clear();
}

void scr_clear(void)
{
    memset(SCREEN + 40, ' ', 960);
    memset(COLRAM + 40, curcolor, 960);
    crow = 1;
    ccol = 0;
    wlen = 0;
    soft = 0;
}

void scroll_up(void)
{
    memmove(SCREEN + 40, SCREEN + 80, 920);
    memmove(COLRAM + 40, COLRAM + 80, 920);
    memset(SCREEN + 960, ' ', 40);
}

unsigned char getkey(void)
{
    unsigned char k;
    while ((k = cbm_k_getin()) == 0)
        ;
    return k;
}

/* Waits at a full screen: SPACE for the next page, RETURN for one more
 * line, Q to stop paging, RUN/STOP to stop the reply. */
static void pager_wait(void)
{
    static const char prompt[] = "-- more -- space, return, q, stop";
    unsigned char *s = SCREEN + crow * 40, *c = COLRAM + crow * 40;
    unsigned char i, k;

    for (i = 0; prompt[i]; ++i) {
        s[i] = a2s[prompt[i]] | 0x80;
        c[i] = C_DIM;
    }
    *(unsigned char *)0xC6 = 0;         /* forget keys typed meanwhile */
    k = getkey();
    memset(s, ' ', 40);
    if (k == ' ') pg_lines = 0;
    else if (k == 13) pg_lines = 22;
    else if (k == 3) { pg_on = 0; pg_quit = 1; }
    else pg_on = 0;
}

static void newline(void)
{
    ccol = 0;
    if (crow < 24) ++crow;
    else scroll_up();
    if (pg_on && ++pg_lines >= 23) pager_wait();
}

static void put_screen(unsigned char sc, unsigned char color)
{
    unsigned int o;
    if (ccol >= 40) {
        newline();
        soft = 1;
    }
    o = crow * 40 + ccol;
    SCREEN[o] = sc;
    COLRAM[o] = color;
    ++ccol;
}

void out_flush(void)
{
    unsigned char i;
    if (!wlen) return;
    if (ccol + wlen > 40 && ccol) {
        newline();
    }
    for (i = 0; i < wlen; ++i) put_screen(wbuf[i], wclr[i]);
    soft = 0;
    wlen = 0;
}

void out_cc(unsigned char c, unsigned char color)
{
    if (c == '\n') {
        out_flush();
        newline();
        soft = 0;
        return;
    }
    if (c == ' ' || c == '\t') {
        out_flush();
        if (ccol >= 40) {               /* the space ends the line */
            newline();
            soft = 1;
        } else if (ccol || !soft) {
            put_screen(' ', color);
            if (c == '\t' && ccol < 40) put_screen(' ', color);
        }
        return;
    }
    if (c < 32 || c > 126) return;
    wbuf[wlen] = a2s[c];
    wclr[wlen] = color;
    if (++wlen == 40) {
        if (ccol) newline();
        out_flush();
    }
}

void out_color(unsigned char c) { curcolor = c; }
void out_c(unsigned char c) { out_cc(c, curcolor); }

void out_s(const char *s)
{
    while (*s) out_cc(*s++, curcolor);
}

void out_u(unsigned long n)
{
    char buf[11];
    out_s(ultoa(n, buf, 10));
}

void out_nl(void) { out_cc('\n', curcolor); }

void out_bol(void)
{
    out_flush();
    if (ccol) newline();
    soft = 0;
}

void out_line(unsigned char color, const char *s)
{
    unsigned char old = curcolor;
    out_bol();
    curcolor = color;
    out_s(s);
    out_nl();
    curcolor = old;
}

/* ---- markdown, a character at a time ---- */

static unsigned char md_ls, md_stars, md_ticks, md_hash;
static unsigned char md_bold, md_code, md_block, md_head, md_skip;

void md_reset(void)
{
    md_ls = 1;
    md_stars = md_ticks = md_hash = 0;
    md_bold = md_code = md_block = md_head = md_skip = 0;
}

static unsigned char md_color(void)
{
    if (md_head) return C_HEAD;
    if (md_block || md_code) return C_CODE;
    if (md_bold) return C_BOLD;
    return curcolor;
}

static void md_pending(void)
{
    for (; md_stars; --md_stars) out_cc('*', md_color());
    for (; md_hash; --md_hash) out_cc('#', md_color());
    for (; md_ticks; --md_ticks) out_cc('`', md_color());
}

void md_c(unsigned char c)
{
    if (!md_on) {
        out_cc(c, curcolor);
        return;
    }
    if (md_skip) {                      /* the rest of a ``` fence line */
        if (c == '\n') {
            md_skip = 0;
            md_ls = 1;
        }
        return;
    }
    if (c == '*' && !md_block && !md_code) {
        if (++md_stars == 2) {
            md_stars = 0;
            md_bold ^= 1;
        }
        return;
    }
    if (md_stars) md_pending();
    if (c == '`') {
        ++md_ticks;
        return;
    }
    if (md_ticks) {
        if (md_ticks >= 3 && md_ls) {
            md_ticks = 0;
            md_block ^= 1;
            md_code = 0;
            if (c != '\n') md_skip = 1;
            return;
        }
        if (md_ticks == 1 && !md_block) {
            md_ticks = 0;
            md_code ^= 1;
        } else {
            md_pending();
        }
    }
    if (md_ls && c == '#' && !md_block) {
        ++md_hash;
        return;
    }
    if (md_hash) {
        if (c == ' ') {
            md_hash = 0;
            md_head = 1;
            md_ls = 0;
            return;
        }
        md_pending();
    }
    if (c == '\n') {
        md_head = md_code = md_bold = 0;
        md_ls = 1;
        out_cc(c, curcolor);
        return;
    }
    md_ls = 0;
    out_cc(c, md_color());
}

void md_end(void)
{
    md_pending();
    out_flush();
}

/* ---- a spinner while waiting for a reply ---- */

static unsigned char spin_on, spin_frame;
static unsigned int spin_last;

void spin_tick(void)
{
    static const unsigned char frames[4] = { 0x2D, 0x1C, 0x5D, 0x2F };   /* - \ | / */
    unsigned int t = ticks();
    if (wlen || ccol >= 40) return;
    if (spin_on && t - spin_last < 8) return;
    spin_last = t;
    spin_on = 1;
    SCREEN[crow * 40 + ccol] = frames[++spin_frame & 3];
    COLRAM[crow * 40 + ccol] = C_DIM;
}

void spin_clear(void)
{
    if (!spin_on) return;
    spin_on = 0;
    if (ccol < 40) SCREEN[crow * 40 + ccol] = ' ';
}

/* ---- keyboard and file name character conversion ---- */

/* PETSCII keyboard code (lower/upper case mode) to ASCII; 0 if none.
 * The pound sign, up arrow, and left arrow keys type \ ^ _ (and show as
 * those); SHIFT with + - @ * gives { } ~ ` and SHIFT+pound gives |. */
unsigned char p2a(unsigned char k)
{
    if (k >= 0x41 && k <= 0x5A) return k + 0x20;
    if (k >= 0xC1 && k <= 0xDA) return k - 0x80;
    if (k >= 0x20 && k <= 0x5F) return k;
    switch (k) {
    case 0xDB: return '{';
    case 0xDD: return '}';
    case 0xA9: return '|';
    case 0xBA: return '~';
    case 0xDE: return '~';
    case 0xC0: return '`';
    }
    return 0;
}

/* ASCII to PETSCII, for disk file names and commands. */
unsigned char a2p(unsigned char c)
{
    if (c >= 0x61 && c <= 0x7A) return c - 0x20;
    if (c >= 0x41 && c <= 0x5A) return c + 0x80;
    return c;
}

/* Asks a yes/no question; true for Y. */
unsigned char confirm(const char *question)
{
    unsigned char k;
    out_bol();
    out_color(C_INFO);
    out_s(question);
    out_s(" (y/N) ");
    out_flush();
    *(unsigned char *)0xC6 = 0;
    k = getkey();
    k = (k == 'Y' || k == 0xD9);        /* PETSCII y, Y */
    out_s(k ? "yes" : "no");
    out_nl();
    out_color(C_TEXT);
    return k;
}
