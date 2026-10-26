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

/* The title, drawn like the skinnyai icon: a thin SKINNY (5 pixels wide
 * and 7 high, one pixel apart) and a bold AI (3-pixel strokes), as 7 characters put in
 * place of block graphics that are otherwise unused. */
static const unsigned char logo[LOGO_CELLS * 8] = {
    0x7A, 0x82, 0x82, 0x73, 0x0A, 0x0A, 0xF2, 0x00,
    0x2A, 0x4B, 0x8B, 0x0A, 0x8A, 0x4A, 0x2A, 0x00,
    0x28, 0x2C, 0x2C, 0xAA, 0x69, 0x69, 0x28, 0x00,
    0xA2, 0xA2, 0x94, 0x88, 0x88, 0x88, 0x88, 0x00,
    0x07, 0x0F, 0x0D, 0x1D, 0x3F, 0x38, 0x70, 0x00,     /* A, a triangle */
    0x03, 0x83, 0x83, 0xC3, 0xE3, 0xE3, 0x73, 0x00,     /* A, and I */
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00,     /* I */
};

/* The title bar's green: seven sprites, stretched to double width and
 * solid for their top 8 lines, across the first text row and behind its
 * characters, so its text can be white (and light green). Their data is
 * in the cassette buffer. */
#define SPRITE_DATA ((unsigned char *)0x0340)
#define SPRITE_PTRS ((unsigned char *)0x07F8)
#define VICREG(n) (*(unsigned char *)(0xD000u + (n)))   /* constant n only: */
/* cc65 -Oirs miscompiles writes through a VIC base pointer after a loop */

static void title_bar(void)
{
    static const unsigned char xlo[7] = { 24, 72, 120, 168, 216, 8, 56 };
    unsigned char i;
    memset(SPRITE_DATA, 0, 64);
    memset(SPRITE_DATA, 0xFF, 24);
    for (i = 0; i < 7; ++i) {
        SPRITE_PTRS[i] = 0x0340 / 64;
        ((unsigned char *)0xD000u)[i * 2] = xlo[i];             /* x: 24 + 48i */
        ((unsigned char *)0xD001u)[i * 2] = 50;                 /* the first text row */
        ((unsigned char *)0xD027u)[i] = C_GREEN;
    }
    VICREG(0x10) = 0x60;                                /* x > 255: sprites 5, 6 */
    VICREG(0x1C) = 0;                                   /* hires */
    VICREG(0x17) = 0;
    VICREG(0x1D) = 0x7F;                                /* double width */
    VICREG(0x1B) = 0x7F;                                /* behind characters */
    VICREG(0x15) = 0x7F;                                /* on */
}

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
    memcpy((unsigned char *)0x0800 + LOGO_CHAR * 8, logo, sizeof logo);
    for (i = 0; i < 128; ++i) a2s[i] = to_screen(i);
    *(unsigned char *)0xD020 = C_GREEN;
    *(unsigned char *)0xD021 = C_BLACK;
    title_bar();
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

/* ---- markdown tables ----
 * A table can't be drawn until its column widths are known, so its lines
 * (those starting with |) are collected, then drawn with the box-drawing
 * characters: columns as wide as their widest cell, narrowed to fit 40
 * columns, with cells wrapped onto more lines as needed. */

#define TBUF     1024
#define MAXCOLS  6      /* more can't fit in 40 columns anyway */
#define CELLMAX  100

#define BOX_H    0x40   /* the screen codes of the line-drawing characters */
#define BOX_V    0x5D
#define BOX_TL   0x70
#define BOX_TR   0x6E
#define BOX_BL   0x6D
#define BOX_BR   0x7D
#define BOX_LT   0x6B   /* ├ */
#define BOX_RT   0x73   /* ┤ */
#define BOX_TT   0x72   /* ┬ */
#define BOX_BT   0x71   /* ┴ */
#define BOX_X    0x5B   /* ┼ */

/* A table's text, until it's complete. Between replies, the line editor
 * keeps the line being typed here while CRSR up looks at earlier ones. */
char tbuf[TBUF];
static unsigned int tlen;
static unsigned char md_table, md_tnl;
static char cell[MAXCOLS][CELLMAX];
static unsigned char clen[MAXCOLS], width[MAXCOLS], minw[MAXCOLS], ncols;

static void table_add(char c)
{
    if (tlen < TBUF - 1) tbuf[tlen++] = c;
}

/* Splits the table line at line into cell[] (with ** and ` removed and
 * spaces trimmed); returns the number of cells, and the line's end. */
static unsigned char split_row(const char *line, const char **end)
{
    unsigned char n = 0, k;
    const char *p = line;
    if (*p == '|') ++p;
    while (*p && *p != '\n') {
        k = 0;
        while (*p == ' ') ++p;
        while (*p && *p != '\n' && *p != '|') {
            if (*p == '\\' && p[1] == '|') ++p;
            else if (*p == '`' || (*p == '*' && p[1] == '*')) {
                p += *p == '*' ? 2 : 1;
                continue;
            }
            if (n < MAXCOLS && k < CELLMAX - 1) cell[n][k++] = *p;
            ++p;
        }
        while (k && cell[n < MAXCOLS ? n : 0][k - 1] == ' ') --k;
        if (n < MAXCOLS) {
            cell[n][k] = 0;
            clen[n] = k;
        }
        ++n;
        if (*p == '|') ++p;
        if (*p == '\n' || !*p) break;   /* a closing | ends the row */
    }
    *end = *p == '\n' ? p + 1 : p;
    return n < MAXCOLS ? n : MAXCOLS;
}

/* The longest word in cell i (up to 12), split at spaces and after -. */
static unsigned char longest_word(unsigned char i)
{
    unsigned char k, run = 0, best = 0;
    for (k = 0; k < clen[i]; ++k) {
        if (cell[i][k] == ' ') {
            run = 0;
            continue;
        }
        if (++run > best) best = run;
        if (cell[i][k] == '-') run = 0;
    }
    return best > 12 ? 12 : best;
}

/* A |---|:--:| line: only -, :, and spaces, with a - in each cell. */
static unsigned char is_rule(unsigned char n)
{
    unsigned char i;
    const char *p;
    for (i = 0; i < n; ++i) {
        if (!strchr(cell[i], '-')) return 0;
        for (p = cell[i]; *p; ++p) {
            if (*p != '-' && *p != ':' && *p != ' ') return 0;
        }
    }
    return n != 0;
}

static void put_box(unsigned char sc)
{
    put_screen(sc, C_BORDER);
}

static void rule_line(unsigned char left, unsigned char mid, unsigned char right)
{
    unsigned char i, j;
    put_box(left);
    for (i = 0; i < ncols; ++i) {
        for (j = 0; j < width[i]; ++j) put_box(BOX_H);
        put_box(i + 1 < ncols ? mid : right);
    }
    newline();
}

/* The start and length of the k-th line of text wrapped at width w. */
static unsigned char wrap_line(const char *s, unsigned char len, unsigned char w,
                               unsigned char k, unsigned char *start)
{
    unsigned char p = 0, n, b;
    for (;;) {
        while (p < len && s[p] == ' ') ++p;
        n = len - p;
        if (n > w) {                    /* break at a space or after a - */
            for (b = w; b && s[p + b] != ' ' && s[p + b - 1] != '-'; --b)
                ;
            n = b ? b : w;
        }
        if (!k--) {
            *start = p;
            return p < len ? n : 0;
        }
        p += n;
    }
}

static unsigned char cell_lines(unsigned char i)
{
    unsigned char k = 0, st;
    while (wrap_line(cell[i], clen[i], width[i], k, &st)) ++k;
    return k ? k : 1;
}

static void row_lines(unsigned char header)
{
    unsigned char i, j, k, h = 1, n, st;
    for (i = 0; i < ncols; ++i) {
        n = cell_lines(i);
        if (n > h) h = n;
    }
    for (k = 0; k < h; ++k) {
        put_box(BOX_V);
        for (i = 0; i < ncols; ++i) {
            n = wrap_line(cell[i], clen[i], width[i], k, &st);
            for (j = 0; j < width[i]; ++j) {
                put_screen(j < n ? a2s[cell[i][st + j] & 127] : ' ', header ? C_BOLD : curcolor);
            }
            put_box(BOX_V);
        }
        newline();
    }
}

static void table_flush(void)
{
    const char *p, *next;
    unsigned char i, n, total, widest, rows = 0, rule_after = 0;

    md_table = md_tnl = 0;
    tbuf[tlen] = 0;
    /* the number of columns and their natural widths */
    ncols = 0;
    memset(width, 1, sizeof width);
    memset(minw, 1, sizeof minw);
    for (p = tbuf; *p; p = next) {
        n = split_row(p, &next);
        if (is_rule(n)) {
            if (rows == 1) rule_after = 1;
            continue;
        }
        ++rows;
        if (n > ncols) ncols = n;
        for (i = 0; i < n; ++i) {
            if (clen[i] > width[i]) width[i] = clen[i];
            total = longest_word(i);
            if (total > minw[i]) minw[i] = total;
        }
    }
    if (!ncols) return;
    /* narrow the widest columns until the table fits: first no narrower
     * than their longest words, then down to 3 if it must */
    for (rows = 0; rows < 2; ++rows) {
        for (;;) {
            total = ncols + 1;
            widest = 255;
            for (i = 0; i < ncols; ++i) {
                total += width[i];
                if (width[i] > (rows ? 3 : minw[i]) && (widest == 255 || width[i] > width[widest])) widest = i;
            }
            if (total <= 40 || widest == 255) break;
            --width[widest];
        }
    }

    out_bol();
    rule_line(BOX_TL, BOX_TT, BOX_TR);
    rows = 0;
    for (p = tbuf; *p; p = next) {
        n = split_row(p, &next);
        if (is_rule(n)) continue;
        for (i = n; i < ncols; ++i) clen[i] = cell[i][0] = 0;
        if (rows == 1 && rule_after) rule_line(BOX_LT, BOX_X, BOX_RT);
        row_lines(rows == 0 && rule_after);
        ++rows;
    }
    rule_line(BOX_BL, BOX_BT, BOX_BR);
}

/* ---- markdown, a character at a time ---- */

static unsigned char md_ls, md_stars, md_ticks, md_hash;
static unsigned char md_bold, md_code, md_block, md_head, md_skip;

void md_reset(void)
{
    md_ls = 1;
    md_table = md_tnl = 0;
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
    if (md_table) {                     /* collecting a table */
        if (!md_tnl) {
            table_add(c);
            if (c == '\n') md_tnl = 1;
            return;
        }
        if (c == '|') {                 /* another row */
            md_tnl = 0;
            table_add(c);
            return;
        }
        table_flush();
        md_ls = 1;
    }
    if (md_ls && c == '|' && !md_block) {
        md_table = 1;
        md_tnl = 0;
        tlen = 0;
        table_add(c);
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
    if (md_table) table_flush();
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
