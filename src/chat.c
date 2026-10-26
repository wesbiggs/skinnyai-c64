/* The conversation: history kept in memory and sent in full each turn (as
 * ollama run does), the streamed reply, and the other server requests. */
#include "ascii.h"
#include <string.h>
#include <stdlib.h>
#include "skinny.h"

/* ---- history ----
 * Far memory (far.s: an REU, or the RAM under I/O and the KERNAL), holding
 * messages one after another: a header (the role, 'u' or 'a' or one of
 * the T_ tool roles in skinny.h, then the text's length and its length as
 * a JSON string, two bytes each), the text, and a 0. With the lengths,
 * going through the messages and working out a request's Content-Length
 * never has to read the text itself. When it fills, the oldest exchange is
 * dropped, never the one in progress (from turn_start on). */

unsigned int hist_len, hist_cap, turn_start;
unsigned char hist_dropped, hist_truncated;
unsigned char context_kb = 32;          /* the REU's history size, in K */
static unsigned int msg_start, msg_len, msg_jlen;

void hist_init(void)
{
    far_init();
    hist_clear();
    hist_limit();
}

void hist_clear(void)
{
    dirty = 0;
    hist_len = turn_start = 0;
    hist_dropped = hist_truncated = 0;
}

static unsigned int hist_word(unsigned int off)
{
    return far_peek(off) | (far_peek(off + 1) << 8);
}

unsigned int hist_next(unsigned int p)
{
    return p + HDR + 1 + hist_word(p + 1);
}

unsigned int hist_count(void)
{
    unsigned int p, n = 0;
    for (p = 0; p < hist_len; p = hist_next(p)) ++n;
    return n;
}

/* Drops the oldest message, and the replies and tool calls that answer it,
 * from before turn_start. */
static unsigned char drop_oldest(void)
{
    unsigned int n;
    unsigned char k;
    for (k = 0; ; ++k) {
        if (!turn_start) return k;
        if (k && hist_peek(0) == 'u') break;
        n = hist_next(0);
        far_move(0, n, hist_len - n);
        hist_len -= n;
        turn_start -= n;
        msg_start -= n;
        hist_dropped = 1;
    }
    return 1;
}

/* Sets the history's size: the REU's (context_kb), or the RAM's 12K;
 * old messages that no longer fit are dropped. */
void hist_limit(void)
{
    hist_cap = reu_banks ? (unsigned int)context_kb << 10 : HIST_TOP - 0xD000u;
    turn_start = hist_len;
    while (hist_len > hist_cap && drop_oldest())
        ;
}

unsigned char hist_begin(unsigned char role)
{
    hist_truncated = 0;
    while (hist_len + HDR + 2 > hist_cap) {
        if (!drop_oldest()) return 0;
    }
    msg_start = hist_len;
    msg_len = msg_jlen = 0;
    far_poke(hist_len, role);
    hist_len += HDR;
    return 1;
}

/* How many characters c takes in a JSON string (net.c's tx_js). */
unsigned char json_size(unsigned char c)
{
    if (c == '"' || c == '\\' || c == '\n') return 2;
    if (c < 32 || c > 126) return 6;
    return 1;
}

void hist_addc(unsigned char c)
{
    if (hist_truncated || !c) return;
    while (hist_len + 2 > hist_cap) {
        if (!drop_oldest()) {
            hist_truncated = 1;
            return;
        }
    }
    far_poke(hist_len++, c);
    ++msg_len;
    msg_jlen += json_size(c);
}

void hist_end(void)
{
    far_poke(hist_len++, 0);
    far_poke(msg_start + 1, msg_len);
    far_poke(msg_start + 2, msg_len >> 8);
    far_poke(msg_start + 3, msg_jlen);
    far_poke(msg_start + 4, msg_jlen >> 8);
}

/* ---- the request ---- */

static unsigned char numeric(const char *s)
{
    if (!strcmp(s, "true") || !strcmp(s, "false")) return 1;
    if (*s == '-') ++s;
    if (!*s) return 0;
    for (; *s; ++s) {
        if ((*s < '0' || *s > '9') && *s != '.') return 0;
    }
    return 1;
}

static void tx_param(unsigned char i)
{
    const char *name = pname[i];
    if (api == API_OPENAI && !strcmp(name, "num_predict")) name = "max_tokens";
    tx_js(name);
    tx_c(':');
    if (!strcmp(name, "stop")) {
        tx_c('[');
        tx_js(pval[i]);
        tx_c(']');
    } else if (numeric(pval[i])) {
        tx_s(pval[i]);
    } else {
        tx_js(pval[i]);
    }
}

static void tx_role(const char *role)
{
    tx_s("{\"role\":\"");
    tx_s(role);
    tx_s("\",\"content\":");
}

/* ---- tools: web_search and fetch_page, for Ollama's cloud models ----
 * A local Ollama that's signed in to ollama.com runs cloud models, and
 * proxies Ollama's web search and fetch APIs over plain HTTP, so the C64
 * can offer the model skinnyai's two tools and run them itself. */

#define TOOL_ROUNDS  5          /* then the model has to answer */
#define SEARCH_TEXT  400        /* characters of each result's page text */
#define FETCH_TEXT   1500

static unsigned char tool_round;
static const char *const tool_names[2] = { "web_search", "fetch_page" };
static const char *const tool_args[2] = { "query", "url" };
static const char *const tool_descs[2] = {
    "Search the web. Returns the top results' titles, URLs, and the start of their text.",
    "Fetch a web page's text, to read a search result. Long pages are cut off."
};

unsigned char tools_on(void)
{
    unsigned char n = strlen(model);
    return api == API_OLLAMA && n > 6 && !strcmp(model + n - 6, "-cloud");
}

static unsigned char offering(void)
{
    return tools_on() && tool_round < TOOL_ROUNDS;
}

static void tx_tool(unsigned char t)
{
    tx_s("{\"type\":\"function\",\"function\":{\"name\":\"");
    tx_s(tool_names[t]);
    tx_s("\",\"description\":\"");
    tx_s(tool_descs[t]);
    if (!t && net_date[0]) {
        tx_s(" Today is ");
        tx_s(net_date);
        tx_c('.');
    }
    tx_s("\",\"parameters\":{\"type\":\"object\",\"properties\":{\"");
    tx_s(tool_args[t]);
    tx_s("\":{\"type\":\"string\"}},\"required\":[\"");
    tx_s(tool_args[t]);
    tx_s("\"]}}}");
}

static void body_chat(void)
{
    unsigned int p;
    unsigned char i, role, t, calls = 0, first = 1;

    tx_s("{\"model\":");
    tx_js(model);
    tx_s(",\"stream\":true,\"messages\":[");
    if (sysmsg[0]) {
        tx_role("system");
        tx_js(sysmsg);
        tx_c('}');
        first = 0;
    }
    for (p = 0; p < hist_len; p = hist_next(p)) {
        role = hist_peek(p);
        t = (role | T_RESULT) == T_FETCH;
        if (is_tool(role) && api != API_OLLAMA) continue;
        if (calls && role != T_SEARCH && role != T_FETCH) {
            tx_s("]}");
            calls = 0;
        }
        if (!first) tx_c(',');
        first = 0;
        if (role == T_SEARCH || role == T_FETCH) {
            /* calls one after another are one message's tool_calls */
            if (!calls) tx_s("{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[");
            calls = 1;
            tx_s("{\"function\":{\"name\":\"");
            tx_s(tool_names[t]);
            tx_s("\",\"arguments\":{\"");
            tx_s(tool_args[t]);
            tx_s("\":");
            tx_jsf(p + HDR, hist_word(p + 3));
            tx_s("}}}");
            continue;
        }
        tx_role(role == 'u' ? "user" : role == 'a' ? "assistant" : "tool");
        tx_jsf(p + HDR, hist_word(p + 3));
        if (is_tool(role)) {
            tx_s(",\"tool_name\":\"");
            tx_s(tool_names[t]);
            tx_c('"');
        }
        tx_c('}');
    }
    if (calls) tx_s("]}");
    tx_c(']');
    if (offering()) {
        tx_s(",\"tools\":[");
        tx_tool(0);
        tx_c(',');
        tx_tool(1);
        tx_c(']');
    }
    if (api == API_OLLAMA) {
        if (nparams) {
            tx_s(",\"options\":{");
            for (i = 0; i < nparams; ++i) {
                if (i) tx_c(',');
                tx_param(i);
            }
            tx_c('}');
        }
        if (keepalive[0]) {
            tx_s(",\"keep_alive\":");
            tx_js(keepalive);
        }
        if (think[0]) {
            tx_s(",\"think\":");
            if (!strcmp(think, "true") || !strcmp(think, "false")) tx_s(think);
            else tx_js(think);
        }
        if (fmtjson) tx_s(",\"format\":\"json\"");
    } else {
        for (i = 0; i < nparams; ++i) {
            tx_c(',');
            tx_param(i);
        }
        if (think[0] && strcmp(think, "true") && strcmp(think, "false")) {
            tx_s(",\"reasoning_effort\":");
            tx_js(think);
        }
        if (fmtjson) tx_s(",\"response_format\":{\"type\":\"json_object\"}");
        if (verbose) tx_s(",\"stream_options\":{\"include_usage\":true}");
    }
    tx_c('}');
}

/* ---- the streamed reply ---- */

#define S_NONE    0
#define S_CONTENT 1
#define S_THINK   2
#define S_ARG     3             /* a tool call's search or URL */

static unsigned char smode, got_content, thinking_open, cut_off;
static unsigned char open_role;         /* the message being added to */
static unsigned long eval_count, prompt_count, eval_ms, total_ms;
static unsigned int t_start;

static void msg_close(void)
{
    if (open_role) hist_end();
    open_role = 0;
}

static void msg_open(unsigned char role)
{
    msg_close();
    if (hist_begin(role)) open_role = role;
    else hist_truncated = 1;
}

static unsigned char chat_begin(void)
{
    smode = S_NONE;
    if (KEY("content")) smode = S_CONTENT;
    else if (KEY("thinking") || KEY("reasoning_content") || KEY("reasoning")) smode = S_THINK;
    else if ((KEY("query") || KEY("url")) && offering()) {
        /* a tool call: its argument is kept as a message of its own */
        smode = S_ARG;
        msg_open(json_key[0] == 'q' ? T_SEARCH : T_FETCH);
    }
    return smode != S_NONE;
}

static void close_thinking(void)
{
    if (!thinking_open) return;
    thinking_open = 0;
    out_bol();
    out_line(C_DIM, "...done thinking.");
    out_nl();
}

static void chat_char(unsigned char c)
{
    if (smode == S_THINK) {
        if (!showthink) return;
        if (!thinking_open) {
            net_spin = 0;
            spin_clear();
            out_line(C_DIM, "Thinking...");
            thinking_open = 1;
        }
        out_cc(c, C_DIM);
        return;
    }
    if (smode == S_ARG) {
        hist_addc(c);
        return;
    }
    if (open_role != 'a') {
        if (c == '\n' || c == ' ') return;     /* leading blank lines */
        close_thinking();
        net_spin = 0;
        spin_clear();
        got_content = 1;
        msg_open('a');
    }
    hist_addc(c);
    md_c(c);
}

/* Milliseconds from a nanosecond count given as a string. */
static unsigned long ns_to_ms(const char *s)
{
    unsigned char n = strlen(s);
    char buf[12];
    if (n <= 6) return 0;
    n -= 6;
    if (n > 10) n = 10;
    memcpy(buf, s, n);
    buf[n] = 0;
    return atol(buf);
}

static void chat_value(unsigned char type)
{
    if (type == 's' && (KEY("error") || KEY("message"))) {
        strncpy(net_err, json_val, sizeof net_err - 1);
    } else if (KEY("done")) {
        if (json_val[0] == 't') net_done = 1;
    } else if (KEY("done_reason") || KEY("finish_reason")) {
        if (!strcmp(json_val, "length")) cut_off = 1;
    } else if (KEY("eval_count") || KEY("completion_tokens")) {
        eval_count = atol(json_val);
    } else if (KEY("prompt_eval_count") || KEY("prompt_tokens")) {
        prompt_count = atol(json_val);
    } else if (KEY("eval_duration")) {
        eval_ms = ns_to_ms(json_val);
    } else if (KEY("total_duration")) {
        total_ms = ns_to_ms(json_val);
    }
}

static void out_tenths(unsigned long tenths)
{
    out_u(tenths / 10);
    out_c('.');
    out_u(tenths % 10);
}

static void print_stats(void)
{
    unsigned long ms = total_ms;
    if (!ms) ms = (unsigned long)(unsigned int)(ticks() - t_start) * 50 / 3;
    out_color(C_DIM);
    out_s("total ");
    out_tenths(ms / 100);
    out_s("s, prompt ");
    out_u(prompt_count);
    out_s(" tokens, reply ");
    out_u(eval_count);
    out_s(" tokens");
    if (eval_ms) {
        out_s(", ");
        out_tenths(eval_count * 10000 / eval_ms);
        out_s(" tokens/s");
    }
    out_s("; received ");
    out_u(net_bytes);
    out_s(" bytes");
    if (net_ticks > 30) {
        out_s(" at ");
        out_u(net_bytes * 60 / net_ticks);
        out_s(" bytes/s");
    }
    out_nl();
    out_color(C_TEXT);
}

static void show_error(int r)
{
    out_bol();
    out_color(C_ERR);
    out_s("Error: ");
    out_s(net_err[0] ? net_err : "no reply");
    if (r == 404 && api == API_OLLAMA && strstr(net_err, "not found")) {
        out_s(" (/list shows the server's models)");
    }
    out_nl();
    out_color(C_TEXT);
}

/* A tool's result, as it streams in: the pages' titles, URLs, and text,
 * with runs of blank space squeezed out, and each piece cut short. */
static unsigned int tool_call, res_left, res_text;
static unsigned char res_prev;

static unsigned char tool_begin(void)
{
    unsigned char any = res_prev != 0;
    if (KEY("title")) {
        res_left = 120;
        if (any) hist_addc('\n');
    } else if (KEY("url")) {
        res_left = 200;
    } else if (KEY("content")) {
        res_left = res_text;
    } else {
        return 0;
    }
    if (any) hist_addc('\n');
    res_prev = '\n';
    return 1;
}

static void tool_char(unsigned char c)
{
    if (!res_left) return;
    if (c == '\t' || c == '\r') c = ' ';
    if (c == ' ' && (res_prev == ' ' || res_prev == '\n')) return;
    if (c == '\n' && res_prev == '\n') return;
    hist_addc(c);
    res_prev = c;
    --res_left;
}

static void body_tool(void)
{
    unsigned char search = hist_peek(tool_call) == T_SEARCH;
    tx_s(search ? "{\"query\":" : "{\"url\":");
    tx_jsf(tool_call + HDR, hist_word(tool_call + 3));
    if (search) tx_s(",\"max_results\":3");
    tx_c('}');
}

static void hist_adds(const char *s)
{
    while (*s) hist_addc(*s++);
}

/* Runs the tool calls in this turn from turn_start + from, each result
 * going after them. Offsets are kept from turn_start, since making room
 * moves the history. Returns how many there were, or TOOL_STOPPED. */
#define TOOL_STOPPED 255
static unsigned char run_tools(unsigned int from)
{
    unsigned int p, t, end = hist_len - turn_start;
    unsigned char role, c, n = 0;
    int r;

    for (; from < end; from = hist_next(p) - turn_start) {
        p = turn_start + from;
        role = hist_peek(p);
        if (role != T_SEARCH && role != T_FETCH) continue;
        ++n;
        out_bol();
        out_color(C_DIM);
        out_s(role == T_SEARCH ? "Searching: " : "Fetching: ");
        for (t = p + HDR; (c = hist_peek(t)); ++t) out_c(c);
        out_nl();

        msg_open(role & ~T_RESULT);
        tool_call = turn_start + from;
        res_text = role == T_SEARCH ? SEARCH_TEXT : FETCH_TEXT;
        res_prev = 0;
        json_on_begin = tool_begin;
        json_on_char = tool_char;
        json_on_value = chat_value;     /* for an error */
        net_spin = 1;
        r = http_request("POST", role == T_SEARCH ? "/api/experimental/web_search" : "/api/experimental/web_fetch", body_tool);
        net_spin = 0;
        spin_clear();
        if (r != 200) {
            /* the model hears about it too */
            if (!net_err[0]) strcpy(net_err, "no reply");
            out_line(C_ERR, net_err);
            hist_adds("Error: ");
            hist_adds(net_err);
        }
        msg_close();
        if (r == NET_ABORT) return TOOL_STOPPED;
        p = turn_start + from;
    }
    return n;
}

void chat_turn(const char *text)
{
    int r;
    unsigned int from;
    unsigned char tools;

    turn_start = hist_len;
    if (!hist_begin('u')) {
        out_line(C_ERR, "That message doesn't fit in memory.");
        return;
    }
    while (*text) hist_addc(*text++);
    if (hist_truncated) {
        hist_len = turn_start;
        out_line(C_ERR, "That message doesn't fit in memory.");
        return;
    }
    hist_end();

    got_content = open_role = tool_round = 0;
    eval_count = prompt_count = eval_ms = total_ms = 0;
    pg_on = pager;
    pg_quit = 0;
    pg_lines = 0;
    hist_dropped = 0;
    out_color(C_TEXT);
    out_nl();                           /* a blank row before the reply */
    t_start = ticks();

    for (;;) {
        smode = thinking_open = cut_off = 0;
        json_on_begin = chat_begin;
        json_on_char = chat_char;
        json_on_value = chat_value;
        md_reset();
        net_spin = 1;
        from = hist_len - turn_start;
        tools = offering();

        r = http_request("POST", api == API_OLLAMA ? "/api/chat" : "/v1/chat/completions", body_chat);

        net_spin = 0;
        spin_clear();
        md_end();
        msg_close();
        if (thinking_open) {
            if (cut_off) out_line(C_ERR, "Cut off: ran out of tokens while still thinking (/set parameter num_predict).");
            close_thinking();
        }
        if (r != 200 || !tools) break;
        tools = run_tools(from);
        if (tools == TOOL_STOPPED) r = NET_ABORT;
        if (!tools || r != 200) break;
        ++tool_round;
        out_color(C_TEXT);
    }
    pg_on = 0;
    out_bol();
    if (got_content && r == NET_ABORT) {
        out_line(C_DIM, "(stopped)");
    } else if (!got_content || r != 200) {
        show_error(r);
        if (!got_content) {
            /* take the question back, so it can be sent again (CRSR up) */
            hist_len = turn_start;
            return;
        }
    } else if (cut_off) {
        out_line(C_ERR, "Cut off: ran out of tokens (/set parameter num_predict).");
    }
    if (hist_truncated) out_line(C_ERR, "(the reply was too long to keep it all in memory)");
    if (hist_dropped) out_line(C_DIM, "(memory is full: the oldest messages were forgotten)");
    dirty = 1;
    if (verbose && r == 200) print_stats();
    out_nl();
    if (autosave && session[0]) {
        if (mf_save(session)) dirty = 0;
        else out_line(C_ERR, dstat);
    }
}

/* ---- /list: the server's models ---- */

static unsigned char list_any;

static unsigned char no_stream(void) { return 0; }
static void ignore_char(unsigned char c) { (void)c; }

static void list_value(unsigned char type)
{
    if (type != 's') return;
    if (KEY(api == API_OLLAMA ? "name" : "id")) {
        out_bol();
        out_s("  ");
        out_s(json_val);
        list_any = 1;
    } else if (KEY("parameter_size") && list_any) {
        out_s(" (");
        out_s(json_val);
        out_c(')');
    } else if (KEY("error") || KEY("message")) {
        strncpy(net_err, json_val, sizeof net_err - 1);
    }
}

static void begin_lookup(void (*value)(unsigned char))
{
    json_on_begin = no_stream;
    json_on_char = ignore_char;
    json_on_value = value;
    pg_on = pager;
    pg_quit = 0;
    pg_lines = 0;
    net_spin = 1;
}

static void end_lookup(int r)
{
    net_spin = 0;
    spin_clear();
    out_bol();
    pg_on = 0;
    if (r != 200) show_error(r);
}

void list_models(void)
{
    int r;
    out_line(C_INFO, "Models on the server:");
    out_color(C_TEXT);
    list_any = 0;
    begin_lookup(list_value);
    r = http_request("GET", api == API_OLLAMA ? "/api/tags" : "/v1/models", 0);
    end_lookup(r);
    if (r == 200 && !list_any) out_line(C_DIM, "  (none)");
}

/* ---- /show info ---- */

static unsigned char caps_seen;

static void show_label(const char *label)
{
    out_bol();
    out_color(C_DIM);
    out_s("  ");
    out_s(label);
    out_s(": ");
    out_color(C_TEXT);
}

static void info_value(unsigned char type)
{
    if (KEY("family") || KEY("parameter_size") || KEY("quantization_level")) {
        show_label(KEY("family") ? "family" : KEY("parameter_size") ? "parameters" : "quantization");
        out_s(json_val);
    } else if (key_ends("context_length") && type == 'l') {
        show_label("context length");
        out_s(json_val);
    } else if (KEY("capabilities") && type == 's') {
        if (!caps_seen) show_label("capabilities");
        else out_s(", ");
        caps_seen = 1;
        out_s(json_val);
    } else if (KEY("error") && type == 's') {
        strncpy(net_err, json_val, sizeof net_err - 1);
    }
}

static void body_show(void)
{
    tx_s("{\"model\":");
    tx_js(model);
    tx_c('}');
}

void show_info(void)
{
    int r;
    if (api != API_OLLAMA) {
        out_line(C_ERR, "/show info needs the Ollama API.");
        return;
    }
    out_line(C_INFO, "Model info (this takes a while):");
    caps_seen = 0;
    begin_lookup(info_value);
    r = http_request("POST", "/api/show", body_show);
    end_lookup(r);
}

/* ---- /page: the latest reply again, a screen at a time ---- */

void show_last_reply(void)
{
    unsigned int p, last = 0xFFFF;
    unsigned char ch;
    for (p = 0; p < hist_len; p = hist_next(p)) {
        if (hist_peek(p) == 'a') last = p;
    }
    if (last == 0xFFFF) {
        out_line(C_DIM, "No reply yet.");
        return;
    }
    scr_clear();
    md_reset();
    pg_on = 1;
    pg_lines = 0;
    pg_quit = 0;
    out_color(C_TEXT);
    for (p = last + HDR; (ch = hist_peek(p)) && !pg_quit; ++p) md_c(ch);
    md_end();
    pg_on = 0;
    out_bol();
    out_nl();
}
