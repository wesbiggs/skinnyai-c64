/* The line editor: RETURN sends, SHIFT+RETURN adds a new line, the cursor
 * keys move and recall earlier lines, DEL deletes, HOME goes to the start,
 * CLR (SHIFT+HOME) empties the line. */
#include "ascii.h"
#include <string.h>
#include <cbm.h>
#include "skinny.h"

char ibuf[INPUT_MAX + 1];
static unsigned int ilen, icur, imax;
static unsigned char srow, scol;

/* Earlier lines, oldest first, each ending in a 0. The buffer is outside
 * the BSS (skinny.cfg's RECALL), since the start-up code runs there until
 * the first line is typed (boot.s). */
#define RECALL_SIZE 896
extern char recall[];
static unsigned int recall_len;

#define NEWLINE_GLYPH 0x6D              /* a corner, like a return arrow */

static void cell(unsigned int i)
{
    unsigned int p = scol + i;
    unsigned int o = (srow + p / 40) * 40 + p % 40;
    unsigned char sc = ' ';
    if (i < ilen) sc = ibuf[i] == '\n' ? NEWLINE_GLYPH : a2s[ibuf[i] & 127];
    if (i == icur) sc ^= 0x80;
    SCREEN[o] = sc;
    COLRAM[o] = C_USER;
}

/* Scrolls so the last cell (the one after the text) is on screen. */
static void make_room(void)
{
    while (srow + (scol + ilen) / 40 > 24) {
        scroll_up();
        --srow;
    }
}

static void draw(unsigned int from, unsigned int to)
{
    make_room();
    for (; from <= to; ++from) cell(from);
}

static void set_text(const char *s)
{
    unsigned int old = ilen;
    ilen = 0;
    icur = 0;
    draw(0, old);                       /* blank the old text */
    ilen = strlen(s);
    if (ilen > imax) ilen = imax;
    memcpy(ibuf, s, ilen);
    icur = ilen;
    draw(0, ilen);
}

static void remember(void)
{
    unsigned int n = ilen + 1, drop;
    if (!ilen || n > RECALL_SIZE) return;
    while (recall_len + n > RECALL_SIZE) {
        drop = strlen(recall) + 1;
        memmove(recall, recall + drop, recall_len - drop);
        recall_len -= drop;
    }
    memcpy(recall + recall_len, ibuf, ilen);
    recall[recall_len + ilen] = 0;
    recall_len += n;
}

/* Start of the entry before offset pos in the recall buffer. */
static unsigned int recall_prev(unsigned int pos)
{
    if (pos) --pos;                     /* the 0 ending the previous entry */
    while (pos && recall[pos - 1]) --pos;
    return pos;
}

/* Reads a line into ibuf (also returned via its length), starting from the
 * text in initial. With recall, CRSR up/down step through earlier lines. */
unsigned int input_line(const char *prompt, const char *initial, unsigned int max, unsigned char recall_on)
{
    unsigned char a;
    unsigned int k;
    unsigned int at = recall_len;       /* recall position: recall_len = the new line */
    char *draft = tbuf;                 /* free while nothing is shown */

    out_bol();
    out_color(C_PROMPT);
    out_s(prompt);
    out_flush();
    out_color(C_USER);
    if (ccol >= 40) {
        scroll_up();
        ccol = 0;
    }
    srow = crow;
    scol = ccol;
    imax = max;
    ilen = 0;
    set_text(initial);
    pg_lines = 0;

    for (;;) {
        k = getkey();
        switch (k) {
        case 13:                        /* RETURN */
            ibuf[ilen] = 0;
            if (recall_on) remember();
            /* show it again, wrapped at words */
            memset(SCREEN + srow * 40, ' ', (25 - srow) * 40);
            crow = srow;
            ccol = 0;
            out_color(C_PROMPT);
            out_s(prompt);
            out_color(C_USER);
            for (k = 0; k < ilen; ++k) out_c(ibuf[k]);
            out_color(C_TEXT);
            out_nl();
            pg_lines = 0;
            return ilen;
        case 141:                       /* SHIFT+RETURN: a new line */
            a = '\n';
            break;
        case 20:                        /* DEL */
            if (icur) {
                --icur;
                memmove(ibuf + icur, ibuf + icur + 1, ilen - icur - 1);
                --ilen;
                draw(icur, ilen + 1);
            }
            continue;
        case 157:                       /* CRSR left */
            if (icur) {
                --icur;
                draw(icur, icur + 1);
            }
            continue;
        case 29:                        /* CRSR right */
            if (icur < ilen) {
                ++icur;
                draw(icur - 1, icur);
            }
            continue;
        case 19:                        /* HOME */
            icur = 0;
            draw(0, ilen);
            continue;
        case 147:                       /* CLR */
            set_text("");
            continue;
        case 145:                       /* CRSR up: an earlier line */
            if (!recall_on || !at) continue;
            if (at == recall_len) {
                ibuf[ilen] = 0;
                strcpy(draft, ibuf);
            }
            at = recall_prev(at);
            set_text(recall + at);
            continue;
        case 17:                        /* CRSR down: a later line */
            if (!recall_on || at == recall_len) continue;
            at += strlen(recall + at) + 1;
            set_text(at == recall_len ? draft : recall + at);
            continue;
        default:
            a = p2a(k);
            if (!a) continue;
        }
        if (ilen >= imax) continue;
        memmove(ibuf + icur + 1, ibuf + icur, ilen - icur);
        ibuf[icur] = a;
        ++ilen;
        ++icur;
        draw(icur - 1, ilen);
    }
}
