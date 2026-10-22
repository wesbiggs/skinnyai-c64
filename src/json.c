/* A streaming JSON tokenizer, fed one byte at a time. It doesn't build
 * anything: for each scalar value it calls json_on_value with the value's
 * key (an array's elements get the array's key) and up to 63 characters of
 * the value; a string whose key json_on_begin accepts is instead streamed
 * character by character to json_on_char, so a reply shows as it arrives.
 * Text outside a top-level object (SSE "data:" prefixes, "NO CARRIER") is
 * ignored. UTF-8 and \u escapes are turned into ASCII approximations. */
#include "ascii.h"
#include <string.h>
#include "skinny.h"

char json_key[24];
char json_val[64];
unsigned char (*json_on_begin)(void);
void (*json_on_char)(unsigned char c);
void (*json_on_value)(unsigned char type);

#define J_OUT 0         /* outside any object */
#define J_WS  1         /* between tokens */
#define J_STR 2
#define J_ESC 3
#define J_HEX 4
#define J_LIT 5

#define MAX_DEPTH 12

/* Shared with json_feed in jsonfast.s, which takes the common case (a
 * plain character inside a string that isn't streamed) itself. */
unsigned char jstate, jis_key, jstreaming, jklen, jvlen;
#define state jstate
#define is_key jis_key
#define streaming jstreaming
#define klen jklen
#define vlen jvlen
static unsigned char depth;
static unsigned char expect_key[MAX_DEPTH + 1];
static unsigned char is_array[MAX_DEPTH + 1];
static char arr_key[MAX_DEPTH + 1][16];
static unsigned char ucount, udrop, hexn;
static unsigned int cp;

void json_reset(void)
{
    state = J_OUT;
    depth = 0;
}

unsigned char key_is(const char *k)
{
    return json_key[0] == k[0] && strcmp(json_key, k) == 0;
}

unsigned char key_ends(const char *k)
{
    unsigned char n = strlen(json_key), m = strlen(k);
    return n >= m && strcmp(json_key + n - m, k) == 0;
}

static void emit(unsigned char c)
{
    if (is_key) {
        if (klen < sizeof json_key - 1) json_key[klen++] = c;
    } else if (streaming) {
        json_on_char(c);
    } else if (vlen < sizeof json_val - 1) {
        json_val[vlen++] = c;
    }
}

static void emit_s(const char *s)
{
    while (*s) emit(*s++);
}

/* Latin-1 letters without their accents, from U+00C0. */
static const char latin1[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

static void emit_cp(void)
{
    if (cp < 0x80) { emit(cp); return; }
    if (cp >= 0xC0 && cp <= 0xFF) { emit(latin1[cp - 0xC0]); return; }
    if (cp >= 0x2500 && cp <= 0x257F) {         /* box drawing */
        if (cp <= 0x2501 || cp == 0x2550) emit('-');
        else if (cp <= 0x2503 || cp == 0x2551) emit('|');
        else emit('+');
        return;
    }
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || cp >= 0xD800) return;
    switch (cp) {
    case 0xA0: emit(' '); return;
    case 0xA9: emit_s("(c)"); return;
    case 0xAE: emit_s("(r)"); return;
    case 0xAB: case 0xBB: case 0x201C: case 0x201D: case 0x201E: case 0x2033:
        emit('"'); return;
    case 0xB0: emit_s(" deg"); return;
    case 0xBC: emit_s("1/4"); return;
    case 0xBD: emit_s("1/2"); return;
    case 0xBE: emit_s("3/4"); return;
    case 0xB1: emit_s("+/-"); return;
    case 0xB7: case 0x2219: emit('.'); return;
    case 0xD7: emit('x'); return;
    case 0xF7: emit('/'); return;
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: emit('\''); return;
    case 0x2022: case 0x25CF: case 0x25E6: case 0x25AA: case 0x2023: emit('*'); return;
    case 0x2026: emit_s("..."); return;
    case 0x2122: emit_s("(tm)"); return;
    case 0x20AC: emit_s("EUR"); return;
    case 0x2190: emit_s("<-"); return;
    case 0x2192: emit_s("->"); return;
    case 0x2194: emit_s("<->"); return;
    case 0x21D2: emit_s("=>"); return;
    case 0x2212: emit('-'); return;
    case 0x2248: emit_s("~="); return;
    case 0x2260: emit_s("!="); return;
    case 0x2264: emit_s("<="); return;
    case 0x2265: emit_s(">="); return;
    case 0x221E: emit_s("inf"); return;
    case 0x2713: case 0x2714: case 0x2705: emit('v'); return;
    case 0x2717: case 0x2718: case 0x274C: emit('x'); return;
    }
    if (cp >= 0x2010 && cp <= 0x2015) { emit('-'); return; }
    if (cp >= 0x2000 && cp <= 0x200A) { emit(' '); return; }
    if (cp >= 0x2600) return;                   /* symbols and pictographs */
    emit('?');
}

static void end_value(unsigned char type)
{
    json_val[vlen] = 0;
    json_on_value(type);
}

void __fastcall__ json_feed_slow(unsigned char c)
{
again:
    switch (state) {
    case J_OUT:
        if (c != '{') return;
        depth = 1;
        expect_key[1] = 1;
        is_array[1] = 0;
        state = J_WS;
        return;

    case J_STR:
        if (c == '"') {
            state = J_WS;
            if (is_key) {
                json_key[klen] = 0;
            } else if (!streaming) {
                end_value('s');
            }
            return;
        }
        if (c == '\\') {
            state = J_ESC;
            return;
        }
        if (c < 0x80) {
            emit(c);
        } else if ((c & 0xC0) == 0x80) {        /* UTF-8 continuation */
            if (ucount) {
                cp = (cp << 6) | (c & 0x3F);
                if (--ucount == 0 && !udrop) emit_cp();
            }
        } else if (c < 0xE0) {
            cp = c & 0x1F; ucount = 1; udrop = 0;
        } else if (c < 0xF0) {
            cp = c & 0x0F; ucount = 2; udrop = 0;
        } else {
            ucount = 3; udrop = 1;              /* beyond U+FFFF: emoji */
        }
        return;

    case J_ESC:
        state = J_STR;
        switch (c) {
        case 'n': emit('\n'); return;
        case 't': emit('\t'); return;
        case 'r': case 'b': case 'f': return;
        case 'u': state = J_HEX; hexn = 0; cp = 0; return;
        }
        emit(c);
        return;

    case J_HEX:
        cp <<= 4;
        if (c >= '0' && c <= '9') cp |= c - '0';
        else cp |= (c | 0x20) - 'a' + 10;
        if (++hexn == 4) {
            state = J_STR;
            emit_cp();
        }
        return;

    case J_LIT:
        if (c == ',' || c == '}' || c == ']' || c <= ' ') {
            state = J_WS;
            end_value('l');
            goto again;
        }
        if (vlen < sizeof json_val - 1) json_val[vlen++] = c;
        return;
    }

    /* J_WS */
    switch (c) {
    case ' ': case '\t': case '\r': case '\n':
        return;
    case '{':
    case '[':
        if (depth < MAX_DEPTH) ++depth;
        is_array[depth] = c == '[';
        expect_key[depth] = c == '{';
        if (c == '[') {
            strncpy(arr_key[depth], json_key, 15);
            arr_key[depth][15] = 0;
        }
        return;
    case '}':
    case ']':
        if (--depth == 0) state = J_OUT;
        return;
    case ',':
        expect_key[depth] = !is_array[depth];
        return;
    case ':':
        expect_key[depth] = 0;
        return;
    case '"':
        state = J_STR;
        ucount = 0;
        if (expect_key[depth]) {
            is_key = 1;
            klen = 0;
            return;
        }
        is_key = 0;
        vlen = 0;
        if (is_array[depth]) strcpy(json_key, arr_key[depth]);
        streaming = json_on_begin();
        return;
    }
    /* the start of a number, true, false, or null */
    if (is_array[depth]) strcpy(json_key, arr_key[depth]);
    state = J_LIT;
    vlen = 1;
    json_val[0] = c;
}
