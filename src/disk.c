/* Saved chats as Modelfiles (SEQ files named <name>.mf, in ASCII with LF
 * line ends, so they open as text elsewhere and `ollama create` takes
 * them), the skinnyai.cfg settings file, and the directory. */
#include "ascii.h"
#include <string.h>
#include <stdlib.h>
#include <cbm.h>
#include "skinny.h"

char dstat[40];
static char fname[32];

#define LFN  2
#define LFNC 15

/* Builds a PETSCII file name: prefix + name + suffix. */
static const char *pet(const char *prefix, const char *name, const char *suffix)
{
    unsigned char i = 0;
    const char *parts[3];
    const char *s;
    unsigned char k;
    parts[0] = prefix;
    parts[1] = name;
    parts[2] = suffix;
    for (k = 0; k < 3; ++k) {
        for (s = parts[k]; *s && i < sizeof fname - 1; ++s) fname[i++] = a2p(*s);
    }
    fname[i] = 0;
    return fname;
}

static unsigned char cmd_open;          /* channel 15 is open */

/* The title bar's sprites take cycles that the KERNAL's serial bus
 * timing can't spare, so they're off while the disk is in use. */
#define SPRITES_ON (*(unsigned char *)0xD015)

static void disk_begin(void)
{
    SPRITES_ON = 0;
}

static unsigned char no_drive(void)
{
    strcpy(dstat, "Drive ");
    utoa(drive, dstat + 6, 10);
    strcat(dstat, " isn't there (/set drive <n>).");
    SPRITES_ON = 0x7F;                  /* the operation ends here */
    return 255;
}

/* Closes the command channel, which closes every file on the drive, so
 * it's kept open for the whole of an operation and closed at the end. */
static void disk_done(void)
{
    if (cmd_open) cbm_close(LFNC);
    cmd_open = 0;
    SPRITES_ON = 0x7F;
}

/* Reads the drive's status into dstat; returns its number (0 = OK). */
static unsigned char status(void)
{
    unsigned char n = 0, c, st;
    dstat[0] = 0;
    disk_begin();
    if (!cmd_open) {
        if (cbm_open(LFNC, drive, 15, "")) {
            cbm_close(LFNC);
            return no_drive();
        }
        cmd_open = 1;
    }
    if (cbm_k_chkin(LFNC) == 0) {
        for (;;) {
            c = cbm_k_basin();
            st = cbm_k_readst();
            if (c == 13 || n >= sizeof dstat - 1) break;
            dstat[n++] = p2a(c) ? p2a(c) : ' ';
            if (st) break;
        }
    }
    dstat[n] = 0;
    cbm_k_clrch();
    if (!n) {
        disk_done();
        return no_drive();
    }
    return atoi(dstat);
}

/* Sends a DOS command (no files may be open). */
static unsigned char command(const char *cmd)
{
    disk_done();
    disk_begin();
    if (cbm_open(LFNC, drive, 15, cmd)) {
        cbm_close(LFNC);
        return no_drive();
    }
    cmd_open = 1;
    return status();
}

static unsigned char open_file(const char *name, const char *suffix)
{
    unsigned char r;
    if (status() == 255) return 0;      /* opens the command channel first */
    r = cbm_open(LFN, drive, LFN, pet("", name, suffix));
    if (r) {
        cbm_close(LFN);
        status();
        return 0;
    }
    if (status() >= 20) {
        cbm_close(LFN);
        return 0;
    }
    return 1;
}

unsigned char valid_name(const char *name)
{
    unsigned char n = strlen(name);
    if (!n || n > 13) return 0;
    for (; *name; ++name) {
        if (strchr(",:*?=\"/@$", *name) || *name < ' ') return 0;
    }
    return 1;
}

/* ---- writing ---- */

static void (*wput)(unsigned char);

static void w_s(const char *s)
{
    while (*s) wput(*s++);
}

/* A triple-quoted value; a literal """ is written as ""\" as skinnyai does. */
static void w_q(const char *s, unsigned char in_far)
{
    unsigned char run = 0, c;
    w_s("\"\"\"");
    for (;; ++s) {
        c = in_far ? far_peek((unsigned int)s) : *s;
        if (!c) break;
        if (c == '"') {
            if (run == 2) {
                wput('\\');
                run = 0;
            } else {
                ++run;
            }
            wput('"');
        } else {
            run = 0;
            wput(c);
        }
    }
    w_s("\"\"\"\n");
}

static void w_setting(const char *name, const char *value)
{
    w_s("# ");
    w_s(name);
    w_s(": ");
    w_s(value);
    wput('\n');
}

/* Writes the session as a Modelfile through put. */
void mf_write(void (*put)(unsigned char))
{
    char num[6];
    unsigned int p;
    unsigned char i;
    wput = put;
    w_s("# Saved by SkinnyAI 64 " VERSION "\n");
    w_setting("api", api == API_OLLAMA ? "ollama" : "openai");
    w_s("# host: http://");
    w_s(host);
    wput(':');
    w_s(utoa(port, num, 10));
    wput('\n');
    if (fmtjson) w_setting("format", "json");
    if (think[0]) w_setting("think", think);
    if (!showthink) w_setting("show thinking", "false");
    if (!md_on) w_setting("markdown", "false");
    if (verbose) w_setting("verbose", "true");
    if (api == API_OLLAMA && keepalive[0]) w_setting("keep-alive", keepalive);
    w_s("FROM ");
    w_s(model);
    wput('\n');
    for (i = 0; i < nparams; ++i) {
        w_s("PARAMETER ");
        w_s(pname[i]);
        wput(' ');
        if (strchr(pval[i], ' ') || strchr(pval[i], '"')) {
            wput('"');
            w_s(pval[i]);
            wput('"');
        } else {
            w_s(pval[i]);
        }
        wput('\n');
    }
    if (sysmsg[0]) {
        w_s("SYSTEM ");
        w_q(sysmsg, 0);
    }
    for (p = 0; p < hist_len; p = hist_next(p)) {
        if (is_tool(hist_peek(p))) continue;    /* a Modelfile can't hold them */
        w_s(hist_peek(p) == 'u' ? "MESSAGE user " : "MESSAGE assistant ");
        w_q((const char *)(p + HDR), 1);
    }
}

static unsigned char werr;

static void disk_put(unsigned char c)
{
    if (!werr) {
        cbm_k_bsout(c);
        if (cbm_k_readst()) werr = 1;
    }
}

unsigned char mf_save(const char *name)
{
    unsigned char r;
    if (command(pet("s0:", name, ".mf")) == 255 || !open_file(name, ".mf,s,w")) {
        disk_done();
        return 0;
    }
    werr = 0;
    cbm_k_ckout(LFN);
    mf_write(disk_put);
    cbm_k_clrch();
    cbm_close(LFN);
    r = status();
    disk_done();
    if (r >= 20 || werr) {
        if (!r) strcpy(dstat, "Write error.");
        return 0;
    }
    dirty = 0;
    return 1;
}

unsigned char mf_exists(const char *name)
{
    unsigned char r = open_file(name, ".mf,s,r");
    if (r) cbm_close(LFN);
    disk_done();
    return r;
}

unsigned char mf_delete(const char *name)
{
    unsigned char r = command(pet("s0:", name, ".mf"));
    disk_done();
    return r == 1;                      /* 01, FILES SCRATCHED */
}

/* ---- reading a Modelfile ----
 * The subset skinnyai writes: # name: value settings comments, FROM,
 * PARAMETER, SYSTEM, MESSAGE, with values either the rest of the line
 * (optionally "quoted") or a """block""" that ends at a line ending in """.
 * Other instructions are skipped. */

#define R_LINE  0       /* start of a line */
#define R_WORD  1       /* the instruction */
#define R_ARG   2       /* single-line arguments, into rline */
#define R_ROLE  3       /* a MESSAGE's role */
#define R_VSTART 4      /* before a SYSTEM/MESSAGE value */
#define R_VQUOTE 5      /* counting the quotes that may open a block */
#define R_BLOCK 6       /* inside """...""" */
#define R_SKIP  7

static unsigned char rstate, rinstr, rquotes, rbs, rlen;
static char rword[12];
static char rline[100];
static unsigned int sys_len;
static unsigned char vsink;     /* 0 none, 1 system message, 2 history */
static char new_host[48];
static unsigned int new_port;
static unsigned char new_api, host_seen;

#define I_FROM    1
#define I_PARAM   2
#define I_SYSTEM  3
#define I_MESSAGE 4
#define I_COMMENT 5

static void sink(unsigned char c)
{
    if (vsink == 1) {
        if (sys_len < SYS_MAX - 1) sysmsg[sys_len++] = c;
    } else if (vsink == 2) {
        hist_addc(c);
    }
}

static void sink_end(void)
{
    if (vsink == 1) sysmsg[sys_len] = 0;
    else if (vsink == 2) hist_end();
    vsink = 0;
}

static void sink_begin(void)
{
    if (rinstr == I_SYSTEM) {
        vsink = 1;
        sys_len = 0;
    } else if (!strcmp(rword, "user") || !strcmp(rword, "assistant")) {
        turn_start = hist_len;
        vsink = hist_begin(rword[0]) ? 2 : 0;
    } else if (!strcmp(rword, "system")) {
        vsink = 1;
        sys_len = 0;
    } else {
        vsink = 0;
    }
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ') ++s;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
    if (*s == '"' && e > s + 1 && e[-1] == '"') {
        e[-1] = 0;
        ++s;
    }
    return s;
}

static void set_host_url(char *v)
{
    char *colon;
    if (!strncmp(v, "http://", 7)) v += 7;
    else if (!strncmp(v, "https://", 8)) return;        /* can't do TLS */
    colon = strchr(v, ':');
    new_port = 80;
    if (colon) {
        *colon = 0;
        new_port = atoi(colon + 1);
    }
    colon = strchr(v, '/');
    if (colon) *colon = 0;
    strncpy(new_host, v, sizeof new_host - 1);
    host_seen = 1;
}

/* A complete single-line instruction or comment, in rline. */
static void line_done(void)
{
    char *v, *sep;
    rline[rlen] = 0;
    if (rinstr == I_COMMENT) {
        sep = strstr(rline, ": ");
        if (!sep) return;
        *sep = 0;
        v = trim(sep + 2);
        sep = trim(rline);
        if (!strcmp(sep, "api")) new_api = strcmp(v, "openai") ? API_OLLAMA : API_OPENAI;
        else if (!strcmp(sep, "host")) set_host_url(v);
        else if (!strcmp(sep, "format")) fmtjson = !strcmp(v, "json");
        else if (!strcmp(sep, "think")) strncpy(think, v, sizeof think - 1);
        else if (!strcmp(sep, "show thinking")) showthink = strcmp(v, "false") != 0;
        else if (!strcmp(sep, "markdown")) md_on = strcmp(v, "false") != 0;
        else if (!strcmp(sep, "verbose")) verbose = !strcmp(v, "true");
        else if (!strcmp(sep, "keep-alive")) strncpy(keepalive, v, sizeof keepalive - 1);
    } else if (rinstr == I_FROM) {
        strncpy(model, trim(rline), sizeof model - 1);
    } else if (rinstr == I_PARAM) {
        v = trim(rline);
        sep = strchr(v, ' ');
        if (sep) {
            *sep = 0;
            set_param(v, trim(sep + 1));
        }
    } else if (rinstr == I_SYSTEM || rinstr == I_MESSAGE) {
        v = trim(rline);
        sink_begin();
        while (*v) sink(*v++);
        sink_end();
    }
}

static void load_byte(unsigned char c)
{
    switch (rstate) {
    case R_LINE:
        if (c == '\n' || c == ' ' || c == '\r') return;
        rlen = 0;
        if (c == '#') {
            rinstr = I_COMMENT;
            rstate = R_ARG;
            return;
        }
        rword[0] = c | 0x20;
        rlen = 1;
        rstate = R_WORD;
        return;
    case R_WORD:
        if (c == ' ' || c == '\n') {
            rword[rlen] = 0;
            rlen = 0;
            rinstr = 0;
            if (!strcmp(rword, "from")) rinstr = I_FROM;
            else if (!strcmp(rword, "parameter")) rinstr = I_PARAM;
            else if (!strcmp(rword, "system")) rinstr = I_SYSTEM;
            else if (!strcmp(rword, "message")) rinstr = I_MESSAGE;
            if (c == '\n') rstate = R_LINE;
            else if (rinstr == I_MESSAGE) rstate = R_ROLE;
            else if (rinstr == I_SYSTEM) rstate = R_VSTART;
            else if (rinstr) rstate = R_ARG;
            else rstate = R_SKIP;
            return;
        }
        if (rlen < sizeof rword - 1) rword[rlen++] = c | 0x20;
        return;
    case R_ROLE:
        if (c == ' ' && !rlen) return;
        if (c == ' ' || c == '\n') {
            rword[rlen] = 0;
            rlen = 0;
            rstate = c == '\n' ? R_LINE : R_VSTART;
            return;
        }
        if (rlen < sizeof rword - 1) rword[rlen++] = c | 0x20;
        return;
    case R_VSTART:
        if (c == ' ') return;
        rquotes = 0;
        rstate = R_VQUOTE;
        /* fall through */
    case R_VQUOTE:
        if (c == '"' && rquotes < 3) {
            if (++rquotes == 3) {
                rstate = R_BLOCK;
                rquotes = 0;
                rbs = 0;
                sink_begin();
            }
            return;
        }
        rlen = 0;
        while (rquotes) {
            rline[rlen++] = '"';
            --rquotes;
        }
        rstate = R_ARG;
        /* fall through */
    case R_ARG:
        if (c == '\n') {
            line_done();
            rstate = R_LINE;
        } else if (rlen < sizeof rline - 1) {
            rline[rlen++] = c;
        }
        return;
    case R_BLOCK:
        if (rbs) {                      /* ""\ seen */
            rbs = 0;
            if (c == '"') {
                sink('"');
                sink('"');
                sink('"');
                return;
            }
            sink('"');
            sink('"');
            sink('\\');
        }
        if (c == '"') {
            ++rquotes;
            return;
        }
        if (c == '\\' && rquotes == 2) {
            rquotes = 0;
            rbs = 1;
            return;
        }
        if (c == '\n' && rquotes >= 3) {
            for (rquotes -= 3; rquotes; --rquotes) sink('"');
            sink_end();
            rstate = R_LINE;
            return;
        }
        if (c == '\r' && rquotes >= 3) return;
        for (; rquotes; --rquotes) sink('"');
        sink(c);
        return;
    case R_SKIP:
        if (c == '\n') rstate = R_LINE;
        return;
    }
}

/* Reads a file into feed, a byte at a time. */
static unsigned char read_file(const char *name, const char *suffix, void (*feed)(unsigned char))
{
    unsigned char c, st;
    if (!open_file(name, suffix)) {
        disk_done();
        return 0;
    }
    cbm_k_chkin(LFN);
    for (;;) {
        c = cbm_k_basin();
        st = cbm_k_readst();
        if (st & 0x80) break;
        feed(c);
        if (st) break;
    }
    cbm_k_clrch();
    cbm_close(LFN);
    disk_done();
    return 1;
}

unsigned char mf_load(const char *name)
{
    if (!mf_exists(name)) return 0;
    hist_clear();
    sysmsg[0] = 0;
    nparams = 0;
    think[0] = 0;
    sys_len = 0;
    vsink = 0;
    rstate = R_LINE;
    host_seen = 0;
    new_api = api;
    read_file(name, ".mf,s,r", load_byte);
    if (rstate == R_BLOCK || rstate == R_ARG) {
        load_byte('\n');
        if (vsink) sink_end();
    }
    turn_start = 0;
    dirty = 0;
    if (new_api != api || (host_seen && (strcmp(new_host, host) || new_port != port))) {
        /* a chat from another server: don't send an API key there unasked */
        if (!apikey[0] || !host_seen || !strcmp(new_host, host) ||
            confirm("This chat used another server. Send your API key there?")) {
            api = new_api;
            if (host_seen) {
                strcpy(host, new_host);
                port = new_port;
            }
        }
    }
    return 1;
}

/* ---- the directory ---- */

void mf_list(void)
{
    unsigned char c, st, line = 0, n, q, any = 0;
    char name[20];
    if (status() == 255 || cbm_open(LFN, drive, 0, pet("$0:*", ".mf", ""))) {
        cbm_close(LFN);
        disk_done();
        out_line(C_ERR, dstat);
        return;
    }
    cbm_k_chkin(LFN);
    cbm_k_basin();                      /* load address */
    cbm_k_basin();
    for (;;) {
        cbm_k_basin();                  /* link */
        c = cbm_k_basin();
        st = cbm_k_readst();
        if (st) break;
        cbm_k_basin();                  /* blocks */
        cbm_k_basin();
        n = q = 0;
        while ((c = cbm_k_basin()) != 0) {
            if (cbm_k_readst()) break;
            if (c == '"') {
                ++q;
            } else if (q == 1 && n < sizeof name - 1) {
                name[n++] = p2a(c) ? p2a(c) : '?';
            }
        }
        name[n] = 0;
        if (line++ && n > 3 && !strcmp(name + n - 3, ".mf")) {
            name[n - 3] = 0;
            out_bol();
            out_s("  ");
            out_s(name);
            any = 1;
        }
        if (cbm_k_readst()) break;
    }
    cbm_k_clrch();
    cbm_close(LFN);
    disk_done();
    out_bol();
    if (!any) out_line(C_DIM, "  (none)");
}

/* ---- skinnyai.cfg: name=value lines ---- */

static void cfg_line(void)
{
    unsigned char k;
    char *eq;
    rline[rlen] = 0;
    eq = strchr(rline, '=');
    if (!eq) return;
    *eq++ = 0;
    if (!strcmp(rline, "host")) strncpy(host, eq, sizeof host - 1);
    else if (!strcmp(rline, "port")) port = atoi(eq);
    else if (!strcmp(rline, "api")) api = strcmp(eq, "openai") ? API_OLLAMA : API_OPENAI;
    else if (!strcmp(rline, "model")) strncpy(model, eq, sizeof model - 1);
    else if (!strcmp(rline, "key")) strncpy(apikey, eq, sizeof apikey - 1);
    else if (!strcmp(rline, "keepalive")) strncpy(keepalive, eq, sizeof keepalive - 1);
    else if (!strcmp(rline, "context")) context_kb = atoi(eq);
    else if (!strcmp(rline, "baud")) {
        for (k = 0; k < 4 && strcmp(eq, bauds[k]); ++k)
            ;
        if (k < 4) acia_baud = k;
    }
    else if (!strcmp(rline, "system")) strncpy(sysmsg, eq, SYS_MAX - 1);
    else if (!strcmp(rline, "pager")) pager = *eq == '1';
    else if (!strcmp(rline, "markdown")) md_on = *eq == '1';
}

static void cfg_byte(unsigned char c)
{
    if (c == '\n' || c == '\r') {
        cfg_line();
        rlen = 0;
    } else if (rlen < sizeof rline - 1) {
        rline[rlen++] = c;
    }
}

unsigned char cfg_load(void)
{
    unsigned char r;
    rlen = 0;
    r = read_file("skinnyai.cfg", ",s,r", cfg_byte);
    if (r && rlen) cfg_line();
    return r;
}

static void cfg_put(const char *name, const char *value)
{
    w_s(name);
    wput('=');
    w_s(value);
    wput('\n');
}

unsigned char cfg_save(void)
{
    char num[6];
    unsigned char r;
    if (command(pet("s0:", "skinnyai.cfg", "")) == 255 || !open_file("skinnyai.cfg", ",s,w")) {
        disk_done();
        return 0;
    }
    werr = 0;
    wput = disk_put;
    cbm_k_ckout(LFN);
    cfg_put("host", host);
    cfg_put("port", utoa(port, num, 10));
    cfg_put("api", api == API_OLLAMA ? "ollama" : "openai");
    cfg_put("model", model);
    if (apikey[0]) cfg_put("key", apikey);
    cfg_put("keepalive", keepalive);
    cfg_put("context", utoa(context_kb, num, 10));
    cfg_put("baud", bauds[acia_baud]);
    cfg_put("pager", pager ? "1" : "0");
    cfg_put("markdown", md_on ? "1" : "0");
    cbm_k_clrch();
    cbm_close(LFN);
    r = status();
    disk_done();
    return r < 20 && !werr;
}
