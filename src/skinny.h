/* SkinnyAI 64: shared declarations. */
#ifndef SKINNY_H
#define SKINNY_H

#define VERSION "0.1"

/* ---- acia.s ---- */
void __fastcall__ acia_init(void);
unsigned char __fastcall__ acia_speed(void);
void __fastcall__ acia_setpoll(unsigned char on);
extern unsigned char acia_baud;         /* 0: 38400, 1: 57600, 2: 115200, 3: 230400 */
void __fastcall__ acia_done(void);
int __fastcall__ acia_get(void);
void __fastcall__ acia_put(unsigned char c);
unsigned char __fastcall__ acia_count(void);
void __fastcall__ acia_flush(void);
unsigned int __fastcall__ ticks(void);
void __fastcall__ charset_init(void);
void __fastcall__ reset(void);

#define SECONDS(n) ((unsigned int)(n) * 60u)

/* ---- settings (main.c) ---- */
#define API_OLLAMA 0
#define API_OPENAI 1
#define SYS_MAX    1024
#define MAX_PARAMS 8

extern char host[48];
extern unsigned int port;
extern char model[48];
extern unsigned char api;
extern char apikey[100];
extern char keepalive[8];
extern char think[8];           /* "" = model default, "true", "false", or a level */
extern unsigned char showthink, verbose, fmtjson, autosave, pager, drive;
extern char sysmsg[SYS_MAX];
extern char pname[MAX_PARAMS][16];
extern char pval[MAX_PARAMS][24];
extern unsigned char nparams;
extern char session[14];        /* name of the saved chat, "" if none */
extern unsigned char dirty;     /* messages since it was saved */

void set_param(const char *name, const char *value);
extern const char *const bauds[4];
void status_bar(void);

/* ---- screen.c ---- */
#define C_BLACK  0
#define C_WHITE  1
#define C_TEXT   15     /* light grey: replies */
#define C_BOLD   1      /* white */
#define C_HEAD   7      /* yellow */
#define C_CODE   3      /* cyan */
#define C_USER   13     /* light green: what you type */
#define C_PROMPT 7      /* yellow */
#define C_BORDER 5      /* green: table lines */
#define C_INFO   14     /* light blue */
#define C_DIM    12     /* grey */
#define C_ERR    10     /* light red */
#define C_GREEN  5

#define LOGO_CHAR  0x61 /* the title's 7 characters, from this screen code */
#define LOGO_CELLS 7
#define LOGO_AI    4    /* where AI starts */

#define SCREEN ((unsigned char *)0x0400)
#define COLRAM ((unsigned char *)0xD800)

extern unsigned char crow, ccol, curcolor;
extern unsigned char a2s[128];
extern unsigned char pg_on, pg_lines, pg_quit;
extern unsigned char md_on;

void scr_init(void);
void scr_clear(void);
void scroll_up(void);
void out_color(unsigned char c);
void out_c(unsigned char c);
void out_cc(unsigned char c, unsigned char color);
void out_s(const char *s);
void out_u(unsigned long n);
void out_flush(void);
void out_nl(void);
void out_line(unsigned char color, const char *s);
void out_bol(void);             /* newline unless at the start of a line */
void md_reset(void);
void md_c(unsigned char c);
void md_end(void);
void spin_tick(void);
void spin_clear(void);
unsigned char getkey(void);
unsigned char p2a(unsigned char k);
unsigned char a2p(unsigned char c);
unsigned char confirm(const char *question);

/* ---- input.c ---- */
#define INPUT_MAX 600
extern char ibuf[INPUT_MAX + 1];
unsigned int input_line(const char *prompt, const char *initial, unsigned int max, unsigned char recall);

/* ---- json.c ---- */
extern char json_key[24];
extern char json_val[64];
extern unsigned char (*json_on_begin)(void);
extern void (*json_on_char)(unsigned char c);
extern void (*json_on_value)(unsigned char type);  /* 's' string, 'l' literal */
void json_reset(void);
void __fastcall__ json_feed(unsigned char c);      /* jsonfast.s */
void __fastcall__ json_feed_slow(unsigned char c);
unsigned char key_is(const char *k);
#define KEY(k) (json_key[0] == (k)[0] && key_is(k))     /* cheaper */
unsigned char key_ends(const char *k);

/* ---- net.c ---- */
#define NET_ABORT   -2
#define NET_TIMEOUT -3
#define NET_NODIAL  -4
#define NET_DROPPED -5

extern unsigned char modem_ok;
extern unsigned char eth_ok;            /* ATC succeeded */
extern unsigned char modem_tr;          /* ATI says TeensyROM: polled */
extern unsigned char baud_fell_back;    /* the saved rate needs a TeensyROM: 38400 */
extern char net_ip[16];                 /* the C64's address, from ATC */
extern unsigned char net_stop;
extern unsigned char net_done;
extern unsigned char net_spin;          /* show a spinner while waiting */
extern char net_err[100];
extern unsigned long net_bytes;         /* the last response's size */
extern unsigned int net_ticks;          /* and how long it took to arrive */
unsigned char net_init(void);
int http_request(const char *method, const char *path, void (*body)(void));
void tx_c(char c);
void tx_s(const char *s);
void tx_js(const char *s);
void tx_u(unsigned long n);

/* ---- far.s: the REU, or RAM under I/O and the KERNAL ---- */
extern unsigned char reu_banks;         /* 64K banks in the REU, 0 if none */
unsigned char __fastcall__ far_init(void);
unsigned char __fastcall__ far_peek(unsigned int off);
void __fastcall__ far_poke(unsigned int off, unsigned char c);
void __fastcall__ far_move(unsigned int dst, unsigned int src, unsigned int n);

/* ---- history (chat.c) ----
 * Messages at offsets in far memory: a header (role, length, JSON length),
 * the text, and a 0. */
#define HDR 5
#ifndef HIST_TOP
#define HIST_TOP  0xFFF8u               /* the end of the RAM history */
#endif
#define hist_peek(off) far_peek(off)
extern unsigned int hist_len, hist_cap, turn_start;
extern unsigned char hist_dropped, hist_truncated;
extern unsigned char context_kb;
void hist_init(void);
void hist_clear(void);
void hist_limit(void);
unsigned char hist_begin(unsigned char role);
void hist_addc(unsigned char c);
void hist_end(void);
unsigned int hist_count(void);
unsigned int hist_next(unsigned int p);
unsigned char json_size(unsigned char c);
void tx_jsf(unsigned int off, unsigned int jlen);

/* ---- chat.c ---- */
void chat_turn(const char *text);
void list_models(void);
void show_info(void);
void show_last_reply(void);

/* ---- disk.c ---- */
extern char dstat[40];
void mf_write(void (*put)(unsigned char));
unsigned char mf_save(const char *name);
unsigned char mf_load(const char *name);
unsigned char mf_exists(const char *name);
unsigned char mf_delete(const char *name);
void mf_list(void);
unsigned char cfg_load(void);
unsigned char cfg_save(void);
unsigned char valid_name(const char *name);

#endif
