/* SkinnyAI 64: a thin chat client for Ollama and OpenAI-compatible servers,
 * for a Commodore 64 with a SwiftLink-compatible network modem (such as the
 * TeensyROM's ethernet).
 * The command set follows skinnyai, which follows `ollama run`. */
#include "ascii.h"
#include <string.h>
#include <stdlib.h>
#include <cbm.h>
#include "skinny.h"

char host[48];
unsigned int port = 11434;
char model[48];
unsigned char api = API_OLLAMA;
char apikey[100];
char keepalive[8] = "1h";
char think[8];
unsigned char showthink = 1, verbose, fmtjson, autosave, pager = 1, drive = 8;
char sysmsg[SYS_MAX];
char pname[MAX_PARAMS][16];
char pval[MAX_PARAMS][24];
unsigned char nparams;
char session[14];
unsigned char dirty;

static char *arg;                       /* the rest of a command line */

/* ---- helpers ---- */

static void info(const char *s) { out_line(C_INFO, s); }
static void err(const char *s) { out_line(C_ERR, s); }

static void label(const char *name, const char *value)
{
    out_bol();
    out_color(C_DIM);
    out_s(name);
    out_color(C_TEXT);
    out_s(value);
    out_nl();
}

/* Splits off the next word of arg, lowercased if lower is set. */
static char *word(unsigned char lower)
{
    char *w;
    while (*arg == ' ') ++arg;
    w = arg;
    while (*arg && *arg != ' ') {
        if (lower && *arg >= 'A' && *arg <= 'Z') *arg |= 0x20;
        ++arg;
    }
    if (*arg) *arg++ = 0;
    while (*arg == ' ') ++arg;
    return w;
}

void set_param(const char *name, const char *value)
{
    unsigned char i;
    for (i = 0; i < nparams && strcmp(pname[i], name); ++i)
        ;
    if (i == MAX_PARAMS) {
        err("Too many parameters.");
        return;
    }
    if (i == nparams) ++nparams;
    strncpy(pname[i], name, sizeof pname[i] - 1);
    strncpy(pval[i], value, sizeof pval[i] - 1);
}

static void set_host(char *v)
{
    char *colon;
    if (!strncmp(v, "http://", 7)) v += 7;
    else if (!strncmp(v, "https://", 8)) {
        err("HTTPS isn't possible on a C64; use a plain http:// server (or a proxy).");
        return;
    }
    colon = strchr(v, '/');
    if (colon) *colon = 0;
    colon = strchr(v, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }
    strncpy(host, v, sizeof host - 1);
}

/* ---- the status bar and welcome ---- */

void status_bar(void)
{
    /* on the green of screen.c's sprites: the logo, and the model in white */
    unsigned char i, n, x;
    for (i = 0; i < 40; ++i) {
        SCREEN[i] = ' ';
        COLRAM[i] = C_WHITE;
    }
    for (i = 0; i < LOGO_CELLS; ++i) {
        SCREEN[i] = LOGO_CHAR + i;
        COLRAM[i] = i < LOGO_AI ? C_WHITE : C_USER;
    }
    n = strlen(model);
    if (n > 32) n = 32;
    x = 40 - n;
    for (i = 0; i < n; ++i) SCREEN[x + i] = a2s[model[i] & 127];
}

static void url(void)
{
    char num[6];
    out_s("http://");
    out_s(host);
    out_c(':');
    out_s(utoa(port, num, 10));
}

/* A row of the welcome table, as markdown for screen.c's table drawing. */
static void row(const char *name, const char *value)
{
    md_c('|');
    while (*name) md_c(*name++);
    md_c('|');
    while (*value) md_c(*value++);
    md_c('|');
    md_c('\n');
}

static void welcome(void)
{
    char num[6], hostport[64];
    unsigned char md = md_on;
    status_bar();
    out_bol();
    out_color(C_TEXT);
    md_on = 1;
    md_reset();
    row("SkinnyAI 64", "v" VERSION);
    row("-", "-");
    row("Model", model[0] ? model : "(none: /set model)");
    if (api == API_OPENAI) row("API", "OpenAI-compatible");
    strcpy(hostport, "http://");
    strcat(hostport, host);
    strcat(hostport, ":");
    strcat(hostport, utoa(port, num, 10));
    row("Host", hostport);
    if (api == API_OLLAMA && keepalive[0]) row("Keep-alive", keepalive);
    row("Drive", utoa(drive, num, 10));
    md_end();
    md_on = md;
    out_color(C_DIM);
    out_s("Type /help for commands. RETURN sends; SHIFT+RETURN adds a line.");
    out_nl();
    out_color(C_TEXT);
}

/* ---- first run ---- */

static void ask(const char *prompt, char *value, unsigned char size)
{
    input_line(prompt, value, size - 1, 0);
    strcpy(value, ibuf);
}

static void setup(void)
{
    char num[8];
    info("Welcome! Let's set up your server.");
    out_color(C_DIM);
    out_s("It must speak plain HTTP: Ollama on your network (start it with OLLAMA_HOST=0.0.0.0) or an OpenAI-compatible server.");
    out_nl();
    do {
        ask("Server address: ", host, sizeof host);
        set_host(host);
    } while (!host[0]);
    utoa(port, num, 10);
    ask("Port: ", num, 6);
    port = atoi(num);
    ask("API (ollama or openai): ", api == API_OLLAMA ? strcpy(num, "ollama") : strcpy(num, "openai"), 8);
    api = num[0] == 'o' && num[1] == 'p' ? API_OPENAI : API_OLLAMA;
    if (api == API_OPENAI && port == 11434) port = 8000;
    ask("Model: ", model, sizeof model);
    if (api == API_OPENAI) ask("API key (RETURN for none): ", apikey, sizeof apikey);
    if (cfg_save()) info("Saved to skinnyai.cfg (/config saves changes).");
    else err(dstat);
}

/* ---- help ---- */

static void help_main(void)
{
    info("Commands:");
    out_color(C_TEXT);
    out_s(
        "/set         Set session variables\n"
        "/show        Show model or session info\n"
        "/load <name> Load a chat (or a model)\n"
        "/save [name] Save the chat to disk\n"
        "/new         Start a new conversation\n"
        "/delete name Delete a saved chat\n"
        "/list        Models and saved chats\n"
        "/page        Page through the last reply\n"
        "/model       Model, host, and API\n"
        "/config      Save settings as defaults\n"
        "/bye         Exit\n"
        "/? set, /? show, /? keys  More help\n");
}

static void help_set(void)
{
    info("/set ...");
    out_color(C_TEXT);
    out_s(
        "model <name>        Switch model\n"
        "host <addr[:port]>  Change server\n"
        "api ollama|openai   Change protocol\n"
        "key <key>           Bearer key (openai)\n"
        "system <text>       System message\n"
        "parameter <n> <v>   e.g. top_k 20\n"
        "format json / noformat\n"
        "think [level] / nothink\n"
        "showthinking / hidethinking\n"
        "verbose / quiet     Token stats\n"
        "markdown / nomarkdown\n"
        "pager / nopager\n"
        "autosave / noautosave\n"
        "keepalive <time>    e.g. 30m (ollama)\n"
        "drive <8-30>        Disk drive\n");
}

static void help_show(void)
{
    info("/show ...");
    out_color(C_TEXT);
    out_s(
        "info        Model details (ollama)\n"
        "settings    This session's settings\n"
        "system      The system message\n"
        "parameters  Parameters you've set\n"
        "modelfile   The chat as a Modelfile\n");
}

static void help_keys(void)
{
    info("Keys:");
    out_color(C_TEXT);
    out_s(
        "RETURN        Send\n"
        "SHIFT+RETURN  New line\n"
        "CRSR up/down  Earlier lines\n"
        "HOME, CLR     Line start, clear\n"
        "RUN/STOP      Stop a reply\n"
        "Pager: SPACE, RETURN, Q (no more)\n"
        "Keys: pound=\\ up-arrow=^ left-arrow=_\n"
        "SHIFT with + - @ * pound: { } ~ ` |\n");
}

/* ---- /show ---- */

static void put_screen_char(unsigned char c) { out_c(c); }

static void show_settings(void)
{
    char num[6];
    label("Model: ", model);
    label("API: ", api == API_OLLAMA ? "ollama" : "openai");
    out_color(C_DIM);
    out_s("Host: ");
    out_color(C_TEXT);
    url();
    out_nl();
    label("Session: ", session[0] ? session : "(not saved)");
    label("Messages: ", utoa(hist_count(), num, 10));
    out_color(C_DIM);
    out_s("Memory: ");
    out_color(C_TEXT);
    out_u(hist_len);
    out_s(" of ");
    out_u(hist_cap);
    out_s(" bytes");
    out_nl();
    label("Think: ", think[0] ? think : "model default");
    label("Show thinking: ", showthink ? "on" : "off");
    label("Format: ", fmtjson ? "json" : "text");
    label("Verbose: ", verbose ? "on" : "off");
    label("Markdown: ", md_on ? "on" : "off");
    label("Pager: ", pager ? "on" : "off");
    label("Autosave: ", autosave ? "on" : "off");
    if (api == API_OLLAMA) label("Keep-alive: ", keepalive[0] ? keepalive : "server default");
    label("API key: ", apikey[0] ? "set" : "none");
    label("Drive: ", utoa(drive, num, 10));
    label("Modem: ", !modem_ok ? "not found" : !eth_ok ? "network not started" : net_ip[0] ? net_ip : "ready");
}

static void cmd_show(void)
{
    char *w = word(1);
    unsigned char i;
    if (!strcmp(w, "info")) show_info();
    else if (!strcmp(w, "settings")) show_settings();
    else if (!strcmp(w, "system")) {
        if (sysmsg[0]) out_line(C_TEXT, sysmsg);
        else info("No system message was specified.");
    } else if (!strcmp(w, "parameters")) {
        if (!nparams) info("No parameters were specified.");
        for (i = 0; i < nparams; ++i) {
            out_bol();
            out_s(pname[i]);
            out_c(' ');
            out_s(pval[i]);
            out_nl();
        }
    } else if (!strcmp(w, "modelfile")) {
        out_color(C_TEXT);
        pg_on = pager;
        pg_lines = 0;
        mf_write(put_screen_char);
        pg_on = 0;
        out_bol();
    } else if (!strcmp(w, "license") || !strcmp(w, "template")) {
        err("Not available on the C64 (too big).");
    } else {
        help_show();
    }
}

/* ---- /set ---- */

static void on_off(const char *what, unsigned char on)
{
    out_bol();
    out_color(C_INFO);
    out_s("Set ");
    out_s(what);
    out_s(on ? " on." : " off.");
    out_nl();
    out_color(C_TEXT);
}

static void cmd_set(void)
{
    char *w = word(1), *v;
    if (!strcmp(w, "system")) {
        strncpy(sysmsg, arg, SYS_MAX - 1);
        info(sysmsg[0] ? "Set system message." : "Cleared system message.");
    } else if (!strcmp(w, "parameter")) {
        v = word(1);
        if (!*v || !*arg) {
            info("Usage: /set parameter <name> <value>");
            info("e.g. temperature, top_p, top_k, seed, num_ctx, num_predict, stop, repeat_penalty");
            return;
        }
        set_param(v, arg);
        info("Set parameter.");
    } else if (!strcmp(w, "format")) {
        fmtjson = !strcmp(word(1), "json");
        on_off("JSON mode", fmtjson);
    } else if (!strcmp(w, "noformat")) {
        fmtjson = 0;
        on_off("JSON mode", 0);
    } else if (!strcmp(w, "verbose") || !strcmp(w, "quiet")) {
        verbose = w[0] == 'v';
        on_off("stats", verbose);
    } else if (!strcmp(w, "think")) {
        v = word(1);
        strcpy(think, *v && strlen(v) < sizeof think ? v : "true");
        on_off("thinking", 1);
    } else if (!strcmp(w, "nothink")) {
        strcpy(think, "false");
        on_off("thinking", 0);
    } else if (!strcmp(w, "showthinking") || !strcmp(w, "hidethinking")) {
        showthink = w[0] == 's';
        on_off("thinking display", showthink);
    } else if (!strcmp(w, "markdown") || !strcmp(w, "nomarkdown")) {
        md_on = w[0] == 'm';
        on_off("markdown", md_on);
    } else if (!strcmp(w, "pager") || !strcmp(w, "nopager")) {
        pager = w[0] == 'p';
        on_off("pager", pager);
    } else if (!strcmp(w, "autosave") || !strcmp(w, "noautosave")) {
        autosave = w[0] == 'a';
        on_off("autosave", autosave);
        if (autosave && !session[0]) info("It starts once the chat has a name (/save <name>).");
    } else if (!strcmp(w, "model")) {
        v = word(0);
        if (!*v) {
            info("Usage: /set model <name>");
            return;
        }
        strncpy(model, v, sizeof model - 1);
        status_bar();
        info("Switched model; the conversation continues.");
    } else if (!strcmp(w, "host")) {
        v = word(0);
        if (!*v) {
            info("Usage: /set host <address[:port]>");
            return;
        }
        if (apikey[0] && !confirm("Your API key will be sent to the new host. OK?")) return;
        set_host(v);
        out_bol();
        out_color(C_INFO);
        out_s("Host: ");
        url();
        out_nl();
    } else if (!strcmp(w, "api")) {
        v = word(1);
        api = !strcmp(v, "openai") ? API_OPENAI : API_OLLAMA;
        info(api == API_OPENAI ? "API: OpenAI-compatible (/v1/chat/completions)." : "API: Ollama (/api/chat).");
    } else if (!strcmp(w, "key")) {
        strncpy(apikey, word(0), sizeof apikey - 1);
        info(apikey[0] ? "Set API key." : "Cleared API key.");
    } else if (!strcmp(w, "keepalive")) {
        strncpy(keepalive, word(0), sizeof keepalive - 1);
        info("Set keep-alive.");
    } else if (!strcmp(w, "drive")) {
        v = word(0);
        if (atoi(v) >= 8 && atoi(v) <= 30) {
            drive = atoi(v);
            info("Set drive.");
        } else {
            info("Usage: /set drive <8-30>");
        }
    } else if (!strcmp(w, "history") || !strcmp(w, "nohistory") || !strcmp(w, "wordwrap") || !strcmp(w, "nowordwrap")) {
        info("Not needed here: words always wrap, and CRSR up recalls earlier lines.");
    } else {
        help_set();
    }
}

/* ---- sessions ---- */

static void cmd_save(void)
{
    char *name = word(0);
    if (!*name) name = session;
    if (!*name) {
        info("Usage: /save <name> (up to 13 characters)");
        return;
    }
    if (!valid_name(name)) {
        err("Names are up to 13 characters, without , : * ? = \" / @ $");
        return;
    }
    if (strcmp(name, session) && mf_exists(name) &&
        !confirm("A chat with that name is saved already. Replace it?")) {
        return;
    }
    out_line(C_DIM, "Saving...");
    if (mf_save(name)) {
        strcpy(session, name);
        out_bol();
        out_color(C_INFO);
        out_s("Saved '");
        out_s(name);
        out_s("' (");
        out_s(name);
        out_s(".mf). Resume it with /load ");
        out_s(name);
        out_nl();
        out_color(C_TEXT);
    } else {
        err(dstat);
    }
}

static void replay(void)
{
    unsigned int p, t = 0, n = 0;
    unsigned char c;
    for (p = 0; p < hist_len; p = hist_next(p)) {
        ++n;
        if (hist_peek(p) == 'u') t = p;
    }
    if (!n) return;
    out_line(C_DIM, n > 2 ? "...the last exchange:" : "The conversation:");
    for (p = t; p < hist_len; p = hist_next(p)) {
        out_bol();
        if (hist_peek(p) == 'u') {
            out_color(C_PROMPT);
            out_s("> ");
            out_color(C_USER);
            for (t = p + 1; (c = hist_peek(t)); ++t) out_c(c);
            out_nl();
            out_nl();
            out_color(C_TEXT);
        } else {
            md_reset();
            for (t = p + 1; (c = hist_peek(t)); ++t) md_c(c);
            md_end();
            out_bol();
        }
    }
    out_nl();
}

static void cmd_load(void)
{
    char *name = word(0);
    if (!*name) {
        info("Saved chats:");
        mf_list();
        return;
    }
    if (valid_name(name)) {
        out_line(C_DIM, "Loading...");
        if (mf_load(name)) {
            strcpy(session, name);
            status_bar();
            out_bol();
            out_color(C_INFO);
            out_s("Loaded '");
            out_s(name);
            out_s("': ");
            out_s(model);
            out_s(", ");
            out_u(hist_count());
            out_s(" messages.");
            out_nl();
            if (hist_dropped) err("(it was too long: the oldest messages were left out)");
            pg_on = pager;
            pg_lines = 0;
            replay();
            pg_on = 0;
            return;
        }
    }
    /* no such chat: like ollama run, /load <model> switches models */
    strncpy(model, name, sizeof model - 1);
    hist_clear();
    session[0] = 0;
    status_bar();
    out_bol();
    out_color(C_INFO);
    out_s("No saved chat by that name; switched to model '");
    out_s(model);
    out_s("' with a new conversation.");
    out_nl();
}

static void cmd_delete(void)
{
    char *name = word(0);
    if (!*name) name = session;
    if (!*name || !valid_name(name)) {
        info("Usage: /delete <name>");
        return;
    }
    if (!mf_exists(name)) {
        err("No saved chat by that name.");
        return;
    }
    if (!confirm("Delete it?")) return;
    if (mf_delete(name)) {
        info("Deleted.");
        if (!strcmp(name, session)) session[0] = 0;
    } else {
        err(dstat);
    }
}

static void cmd_model(void)
{
    label("Model: ", model);
    if (api == API_OLLAMA) label("Keep-alive: ", keepalive[0] ? keepalive : "server default");
    out_color(C_DIM);
    out_s("Host: ");
    out_color(C_TEXT);
    url();
    out_nl();
    label("API: ", api == API_OLLAMA ? "ollama" : "openai");
}

/* ---- the loop ---- */

static unsigned char command(char *line)
{
    char *cmd;
    arg = line + 1;
    cmd = word(1);
    if (!strcmp(cmd, "bye") || !strcmp(cmd, "exit") || !strcmp(cmd, "quit")) return 0;
    if (!strcmp(cmd, "set")) cmd_set();
    else if (!strcmp(cmd, "show")) cmd_show();
    else if (!strcmp(cmd, "save")) cmd_save();
    else if (!strcmp(cmd, "load")) cmd_load();
    else if (!strcmp(cmd, "new") || !strcmp(cmd, "clear")) {
        hist_clear();
        session[0] = 0;
        info("Started a new conversation (the system message stays).");
    } else if (!strcmp(cmd, "delete")) cmd_delete();
    else if (!strcmp(cmd, "list")) {
        list_models();
        info("Saved chats:");
        mf_list();
    } else if (!strcmp(cmd, "page")) show_last_reply();
    else if (!strcmp(cmd, "model")) cmd_model();
    else if (!strcmp(cmd, "config")) {
        if (cfg_save()) info("Saved these settings to skinnyai.cfg.");
        else err(dstat);
    } else if (!strcmp(cmd, "cls")) {
        scr_clear();
    } else if (!strcmp(cmd, "?") || !strcmp(cmd, "help")) {
        cmd = word(1);
        if (!strcmp(cmd, "set")) help_set();
        else if (!strcmp(cmd, "show")) help_show();
        else if (!strcmp(cmd, "keys") || !strcmp(cmd, "shortcuts")) help_keys();
        else help_main();
    } else {
        out_bol();
        out_color(C_ERR);
        out_s("Unknown command '/");
        out_s(cmd);
        out_s("'. Type /? for help.");
        out_nl();
    }
    out_color(C_TEXT);
    return 1;
}

int main(void)
{
    unsigned char k;
    unsigned char *last_drive = (unsigned char *)0xBA;

    if (*last_drive >= 8 && *last_drive <= 30) drive = *last_drive;
    scr_init();
    status_bar();
    hist_init();
    out_line(C_DIM, "Reading skinnyai.cfg...");
    if (!cfg_load()) {
        scr_clear();
        setup();
    }
    scr_clear();
    welcome();
    if (!net_init()) {
        err("No modem answered on the SwiftLink at $DE00 (on a TeensyROM: Special IO, Swift-Turbo/Modem). Restart once it's on; saved chats still work.");
    } else if (!eth_ok) {
        err("The network didn't start (ATC); check the cable and the modem's network settings. Messages will try again.");
    }
    out_nl();

    for (;;) {
        if (!input_line("> ", "", INPUT_MAX, 1)) continue;
        if (ibuf[0] == '/' && ibuf[1] != '/') {
            if (!command(ibuf)) break;
        } else {
            chat_turn(ibuf[0] == '/' ? ibuf + 1 : ibuf);
        }
    }

    if (dirty && hist_len) {
        k = confirm(session[0] ? "Save the chat first?" : "Save the chat first? It needs a name");
        if (k) {
            if (!session[0]) {
                input_line("Name: ", "", 13, 0);
                if (valid_name(ibuf)) strcpy(session, ibuf);
            }
            if (!session[0] || !mf_save(session)) err(session[0] ? dstat : "Not saved.");
        }
    }
    acia_done();
    reset();
    return 0;
}
