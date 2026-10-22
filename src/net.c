/* The TeensyROM's ethernet "modem" and HTTP/1.0 over it.
 *
 * In command mode the modem takes AT commands (ATC brings up ethernet,
 * ATDT host:port opens a TCP connection) and answers with result words
 * such as OK, CONNECT, or NO ANSWER, each ending in a CR. Once connected,
 * bytes pass through untouched until the server closes the connection,
 * when the modem sends "NO CARRIER" and goes back to command mode. An
 * HTTP/1.0 server closes the connection after its response, so that is
 * the end of every exchange. "+++" with a second's quiet on either side
 * hangs up early. */
#include "ascii.h"
#include <string.h>
#include <stdlib.h>
#include "skinny.h"

unsigned char modem_ok;
unsigned char net_stop;
unsigned char net_done;
unsigned char net_spin;
char net_err[100];
unsigned long net_bytes;
unsigned int net_ticks;

static unsigned int last_tx;
static char mline[81];
static unsigned char mlen;

#define R_OK         0
#define R_CONNECT    1
#define R_NOCARRIER  3
#define R_ERROR      4
#define R_NODIALTONE 6
#define R_BUSY       7
#define R_NOANSWER   8

/* RUN/STOP: the KERNAL's flag, or the key waiting in the keyboard buffer. */
static unsigned char stop_pressed(void)
{
    unsigned char i, n = *(unsigned char *)0xC6;
    if (*(unsigned char *)0x91 == 0x7F || pg_quit) goto stop;
    for (i = 0; i < n; ++i) {
        if (((unsigned char *)0x277)[i] == 3) goto stop;
    }
    return 0;
stop:
    *(unsigned char *)0xC6 = 0;
    return 1;
}

static void put_s(const char *s)
{
    while (*s) acia_put(*s++);
    last_tx = ticks();
}

/* Reads one non-empty line from the modem; false on timeout or RUN/STOP. */
static unsigned char read_line(unsigned int timeout)
{
    unsigned int t0 = ticks();
    int c;
    mlen = 0;
    for (;;) {
        c = acia_get();
        if (c < 0) {
            if (ticks() - t0 > timeout || stop_pressed()) return 0;
            continue;
        }
        if (c == '\r') {
            if (mlen) break;
            continue;
        }
        if (c == '\n') continue;
        if (mlen < 80) mline[mlen++] = c;
    }
    mline[mlen] = 0;
    return 1;
}

/* The result code a line holds (verbose words or numeric), or -1. */
static int result_of(void)
{
    static const char *const words[] = {
        "OK", "CONNECT", "RING", "NO CARRIER", "ERROR",
        "CONNECT 1200", "NO DIALTONE", "BUSY", "NO ANSWER"
    };
    unsigned char i;
    if (mlen == 1 && mline[0] >= '0' && mline[0] <= '8') return mline[0] - '0';
    if (strncmp(mline, "CONNECT", 7) == 0) return R_CONNECT;
    for (i = 0; i < 9; ++i) {
        if (strcmp(mline, words[i]) == 0) return i;
    }
    return -1;
}

/* Sends an AT command and waits for its result; other lines (the modem's
 * own messages) are shown when show is set. -1 on timeout. */
static int modem_cmd(const char *cmd, unsigned int timeout, unsigned char show)
{
    int r;
    unsigned char i;
    put_s(cmd);
    put_s("\r");
    while (read_line(timeout)) {
        r = result_of();
        if (r >= 0) return r;
        if (show && strncmp(mline, cmd, 3) != 0) {
            for (i = 0; i < mlen; ++i) mline[i] = p2a(mline[i]) ? p2a(mline[i]) : ' ';
            out_line(C_DIM, mline);
        }
    }
    return -1;
}

/* Waits for "NO CARRIER" (or for timeout ticks of quiet), discarding
 * what arrives; the end of a body runs right into it, with no CR before. */
static void drain(unsigned int timeout)
{
    static const char nocarrier[] = "NO CARRIER\r";
    unsigned char nc = 0;
    unsigned int t0 = ticks();
    int c;
    for (;;) {
        c = acia_get();
        if (c < 0) {
            if (ticks() - t0 > timeout) return;
            continue;
        }
        t0 = ticks();
        if (c == nocarrier[nc]) {
            if (++nc == sizeof nocarrier - 1) return;
        } else {
            nc = c == 'N';
        }
    }
}

static void hangup(void)
{
    while (ticks() - last_tx < 70)      /* guard time before +++ */
        acia_flush();
    put_s("+++");
    while (ticks() - last_tx < 75)      /* and after */
        ;
    drain(SECONDS(3));
    acia_flush();
}

/* Checks for the modem and brings up ethernet. */
unsigned char net_init(void)
{
    int r;
    acia_init();
    put_s("\r");
    r = modem_cmd("AT", SECONDS(2), 0);
    if (r < 0) {                        /* maybe still online from before */
        hangup();
        r = modem_cmd("AT", SECONDS(2), 0);
    }
    if (r < 0) return modem_ok = 0;
    modem_cmd("ATE0", SECONDS(2), 0);
    modem_cmd("ATV1", SECONDS(2), 0);
    out_line(C_DIM, "Starting ethernet (ATC)...");
    r = modem_cmd("ATC", SECONDS(30), 1);
    modem_ok = 1;
    if (r != R_OK) {
        out_line(C_ERR, "Ethernet didn't start; check the cable and the TeensyROM's network settings.");
    }
    return 1;
}

static unsigned char dial(void)
{
    char cmd[70], num[6];
    int r;
    unsigned char i;
    /* make sure the modem is listening: a +++ sent just after the server
     * hung up is a bad command, so its ERROR may come first */
    acia_flush();
    put_s("\r");
    drain(15);
    for (i = 0; (r = modem_cmd("AT", SECONDS(2), 0)) != R_OK && i < 2; ++i) {
        if (r < 0) hangup();
    }
    strcpy(cmd, "ATDT");
    strcat(cmd, host);
    strcat(cmd, ":");
    strcat(cmd, utoa(port, num, 10));
    r = modem_cmd(cmd, SECONDS(30), 0);
    if (r == R_CONNECT) return 1;
    switch (r) {
    case R_NOANSWER:
        strcpy(net_err, "The server didn't answer. Is it running, and listening on the network (OLLAMA_HOST=0.0.0.0)?");
        break;
    case R_NODIALTONE:
        strcpy(net_err, "No network: ethernet isn't connected.");
        break;
    case -1:
        strcpy(net_err, net_stop ? "Stopped." : "The modem didn't respond.");
        break;
    default:
        strcpy(net_err, "The modem said ");
        strcat(net_err, mline);
    }
    return 0;
}

/* ---- request bodies: counted once for Content-Length, then sent ---- */

static unsigned char counting;
static unsigned long body_len;

void tx_c(char c)
{
    if (counting) ++body_len;
    else acia_put(c);
}

void tx_s(const char *s)
{
    while (*s) tx_c(*s++);
}

void tx_u(unsigned long n)
{
    char buf[11];
    tx_s(ultoa(n, buf, 10));
}

/* A JSON string, quoted and escaped, from memory or (tx_jsf) far memory. */
static unsigned char far_src;
static const char *src;

static unsigned char next_src(void)
{
    return far_src ? far_peek((unsigned int)src++) : *src++;
}

static void tx_jstr(void)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char c;
    tx_c('"');
    while ((c = next_src())) {
        if (c == '"' || c == '\\') {
            tx_c('\\');
            tx_c(c);
        } else if (c == '\n') {
            tx_s("\\n");
        } else if (c < 32 || c > 126) {
            tx_s("\\u00");
            tx_c(hex[c >> 4]);
            tx_c(hex[c & 15]);
        } else {
            tx_c(c);
        }
    }
    tx_c('"');
}

void tx_js(const char *s)
{
    far_src = 0;
    src = s;
    tx_jstr();
}

void tx_jsf(unsigned int addr)
{
    far_src = 1;
    src = (const char *)addr;
    tx_jstr();
}

/* ---- the response ---- */

#define H_STATUS 0
#define H_HEADER 1
#define H_BODY   2
#define H_CSIZE  3
#define H_CDATA  4
#define H_CEND   5
#define H_DONE   6

static unsigned char hstate, chunked, has_len, fast, cext;
extern unsigned char rx_nc, rx_lo;
extern unsigned int rx_hi;
unsigned char __fastcall__ rx_pump(void);
static unsigned long remaining, clen;
static int status;
static char hline[64];
static unsigned char hlen;

static void header_line(void)
{
    unsigned char i;
    hline[hlen] = 0;
    if (hstate == H_STATUS) {
        status = strncmp(hline, "HTTP/", 5) == 0 ? atoi(hline + 9) : 0;
        hstate = H_HEADER;
        return;
    }
    if (!hlen) {                        /* the blank line before the body */
        if (chunked) {
            hstate = H_CSIZE;
            remaining = 0;
        } else {
            hstate = has_len && !clen ? H_DONE : H_BODY;
            remaining = clen;
            fast = hstate == H_BODY && !has_len && status == 200;
        }
        return;
    }
    for (i = 0; i < hlen && hline[i] != ':'; ++i) {
        if (hline[i] >= 'A' && hline[i] <= 'Z') hline[i] |= 0x20;
    }
    if (strncmp(hline, "content-length:", 15) == 0) {
        has_len = 1;
        clen = atol(hline + 15);
    } else if (strncmp(hline, "transfer-encoding:", 18) == 0 && strstr(hline, "hunked")) {
        chunked = 1;
    }
}

static unsigned char hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 255;
}

static void body_byte(unsigned char c)
{
    json_feed(c);
    if (status != 200 && !net_err[0] && hlen < 60 && c >= ' ') {
        hline[hlen++] = c;              /* the start of an error page */
    }
}

static void http_byte(unsigned char c)
{
    unsigned char v;
    switch (hstate) {
    case H_STATUS:
    case H_HEADER:
        if (c == '\n') {
            header_line();
            hlen = 0;
        } else if (c != '\r' && hlen < sizeof hline - 1) {
            hline[hlen++] = c;
        }
        return;
    case H_BODY:
        body_byte(c);
        if (has_len && --remaining == 0) hstate = H_DONE;
        return;
    case H_CSIZE:
        if (c == '\n') {
            hstate = remaining ? H_CDATA : H_DONE;
            cext = 0;
            return;
        }
        v = hexval(c);
        if (v == 255) cext = 1;         /* ;extensions and the CR */
        else if (!cext) remaining = (remaining << 4) | v;
        return;
    case H_CDATA:
        body_byte(c);
        if (--remaining == 0) hstate = H_CEND;
        return;
    case H_CEND:
        if (c == '\n') {
            hstate = H_CSIZE;
            remaining = 0;
        }
        return;
    }
}

/* Makes one HTTP/1.0 request to host:port. The body function (or NULL for
 * none) writes the JSON with tx_*; the response body goes to the JSON
 * tokenizer, whose callbacks the caller has set. Returns the HTTP status,
 * or a NET_ error with the reason in net_err. RUN/STOP stops it. */
int http_request(const char *method, const char *path, void (*body)(void))
{
    static const char nocarrier[] = "NO CARRIER\r";
    unsigned char n, r, got = 0;
    unsigned int t, t0, idle, wait = SECONDS(300);
    int c;

    net_err[0] = 0;
    net_stop = 0;
    net_done = 0;
    if (!modem_ok) {
        strcpy(net_err, "No modem: set the TeensyROM's Special IO to Swift-Turbo/Modem.");
        return NET_NODIAL;
    }
    if (!dial()) return NET_NODIAL;

    body_len = 0;
    if (body) {
        counting = 1;
        body();
        counting = 0;
    }
    tx_s(method);
    tx_c(' ');
    tx_s(path);
    tx_s(" HTTP/1.0\r\nHost: ");
    tx_s(host);
    tx_c(':');
    tx_u(port);
    tx_s("\r\nUser-Agent: SkinnyAI64/" VERSION "\r\nAccept: */*\r\n");
    if (apikey[0]) {
        tx_s("Authorization: Bearer ");
        tx_s(apikey);
        tx_s("\r\n");
    }
    if (body) {
        tx_s("Content-Type: application/json\r\nContent-Length: ");
        tx_u(body_len);
        tx_s("\r\n\r\n");
        body();
    } else {
        tx_s("\r\n");
    }
    last_tx = ticks();

    hstate = H_STATUS;
    hlen = 0;
    chunked = has_len = fast = cext = 0;
    status = 0;
    json_reset();
    idle = ticks();
    rx_lo = rx_nc = 0;
    rx_hi = 0;
    t0 = 0;

    for (;;) {
        if (fast) {
            /* the plain body of a response: the assembly loop */
            n = rx_lo;
            r = rx_pump();
            if (r == 1) break;
            if (rx_lo != n) got = 1;
            if (r == 2) {
                if (stop_pressed() || net_stop) goto stop;
                continue;
            }
        } else {
            c = acia_get();
            if (c >= 0) {
                if (!++rx_lo) ++rx_hi;
                got = 1;
                if (c == nocarrier[rx_nc]) {
                    if (++rx_nc == sizeof nocarrier - 1) break;
                } else {
                    rx_nc = c == 'N';
                }
                if (hstate != H_DONE) {
                    http_byte(c);
                    if (hstate >= H_BODY) {
                        wait = SECONDS(120);
                        if (!t0) t0 = ticks();
                    }
                }
                if (!(rx_lo & 63) && (stop_pressed() || net_stop)) goto stop;
                continue;
            }
        }
        /* nothing has arrived */
        t = ticks();
        if (got) {
            idle = t;
            got = 0;
        }
        if (stop_pressed() || net_stop) goto stop;
        if (hstate == H_DONE || net_done) {
            /* the response is complete: the server should hang up */
            if (t - idle > SECONDS(2)) {
                hangup();
                break;
            }
        } else if (t - idle > wait) {
            hangup();
            strcpy(net_err, "Timed out waiting for the server.");
            return NET_TIMEOUT;
        }
        if (net_spin) spin_tick();
    }
    net_ticks = ticks() - t0;
    net_bytes = ((unsigned long)rx_hi << 8) | rx_lo;
    spin_clear();
    acia_flush();
    if (hstate < H_BODY) {
        strcpy(net_err, "The server closed the connection without answering.");
        return NET_DROPPED;
    }
    if (status != 200 && !net_err[0]) {
        hline[hlen] = 0;
        strcpy(net_err, "HTTP ");
        utoa(status, net_err + 5, 10);
        if (hlen) {
            strcat(net_err, ": ");
            strcat(net_err, hline);
        }
    }
    return status;

stop:
    hangup();
    strcpy(net_err, "Stopped.");
    return NET_ABORT;
}
