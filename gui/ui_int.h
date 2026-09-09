#ifndef WHIMSY_UI_INT_H
#define WHIMSY_UI_INT_H

#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ui.h"

#include "cmd.h"
#include "conf.h"
#include "draw.h"
#include "field.h"
#include "react.h"
#include "whimsy.h"

/* ui.c and its ui_*.c siblings share one struct; nothing outside gui/ sees it */

#define SIDE_MIN 140
#define MEMB_MIN 140
#define MID_MIN  360           /* 140 + 140 + 360 = the 640 px floor with both panes open */
#define WIDE     900            /* below this the sidebar is a header toggle */
#define MEMB_AT 1100            /* below this the members pane is an overlay */
#define AV       28             /* message avatar diameter */
#define GROUP_SECS 300          /* rows from one sender this close share a header */
#define COMP_LINES 8
#define MAXWRAP 256
#define MIDPAD  12             /* header, log and composer share one inset */
#define POP_ROWS 8              /* popup rows shown at once */
#define INFO_MAX 12
#define SET_W   900            /* the settings column, categories included */
#define SET_CATS 180           /* its category column, mockups/mockup.html .set */
#define SET_FONTS 8             /* suggestion rows under a font field */
#define SET_KEY 150            /* its key column */
#define AVATAR_PX 64            /* .map/gui.md: avatars are 64 px, at most 8k */
#define AVATARS 32              /* avatar textures held at once, least used dropped */
#define LSPANS 512              /* text segments of one painted log frame, for selection */

enum { H_GROUP = 1, H_CHAN, H_SIDE, H_MEMB, H_COMP, H_SDRAG, H_MDRAG,
       H_ACT, H_RCT, H_PILL, H_GOTO, H_PROF, H_CARD, H_CBTN, H_NEWPILL, H_ZOOM,
       H_UNARM, H_SROW, H_SFONT, H_SCAT, H_LINK, H_SPOIL,
       H_CDRAG, H_CZDRAG, H_COK, H_CCANCEL, H_SLOT, H_AUDIO, H_SEL };
enum { A_REPLY = 1, A_EDIT, A_DEL, A_COPY, A_SAVE, A_FCOPY };
/* what a click armed the composer with; the bar above the composer names it */
enum { M_NONE, M_REPLY, M_EDIT, M_DEL };
enum { OV_NONE, OV_IMAGE, OV_TEXT };

static const uint8_t ok_col[3]  = {143, 181, 154};
static const uint8_t bad_col[3] = {192, 138, 142};

struct hit { float x, y, w, h; int kind; size_t a, b; };
/* one styled run of one wrapped log line, as it was painted: the log selection maps a
 * pointer back to a byte of m.text through these */
struct lspan { size_t row, at, len; float x, y, w, h; int font; };

struct ui {
	struct whimsy *w;
	struct draw *d;
	struct conf *c;
	SDL_Renderer *r;
	uint8_t self[WHIMSY_PK];

	size_t g, chan;                 /* active group, active channel row */
	int side_open, memb_open;       /* memb_open holds at every width */
	float side_w, memb_w;           /* dragged pane widths, clamped at paint */
	float win_w;                    /* last painted width, for drag arithmetic */
	float scale;                    /* last painted display scale, for a `:set` font reload */
	int drag;                       /* H_SDRAG or H_MDRAG while a splitter is held */
	float scroll;                   /* px scrolled up from the newest row */
	int mode;                       /* M_REPLY, M_EDIT or M_DEL, standing on mode_i */
	size_t mode_i;
	size_t act_i;                   /* the row a click last named, (size_t)-1 for none */
	size_t newat;                   /* the row the `new` rule sits above, (size_t)-1 */
	size_t spoil;                   /* the row whose spoilers are uncovered, (size_t)-1 */
	float mx, my;                   /* last mouse position, for the hover strip */
	float log_y, log_h;             /* the log viewport, for the unread landing */
	int card;                       /* the profile card is open on card_pk */
	uint8_t card_pk[WHIMSY_PK];
	int ask_typing;                 /* the consent question is standing */
	int over;                       /* OV_IMAGE or OV_TEXT, on row over_i */
	size_t over_i;
	int dialog;                     /* 1 a file to send, 2 where to save, 3 an avatar */
	/* the avatar crop screen: the picked picture while it stands. offsets are in
	 * radii, so the circle can be any size the window allows */
	SDL_Texture *cr_t, *cr_dst;
	char cr_path[512];
	int cr_w, cr_h;                 /* the picture's decoded size */
	int cr_grp;                     /* the group's avatar, not ours */
	int cr_drag;                    /* 1 panning the picture, 2 on the slider */
	float cr_zoom, cr_ox, cr_oy;
	float cr_r, cr_sx, cr_sw;       /* last painted circle and slider, for a drag */
	int set_open;                   /* the settings screen, drawn instead of the panes */
	int set_row;                    /* the row it is editing in place, -1 for none */
	int set_cat;                    /* the category column's selection */
	float set_scroll;
	struct field set_f;
	char set_font[SET_FONTS][DRAW_FONT_MAX];
	int set_nfont, set_fsel;        /* the suggestions under a font field */
	/* one decoded texture per held file; a slot goes when whimsy_file_open stops
	 * handing the bytes over. a taken slot has b set and t NULL when the bytes did not
	 * decode: without that the log re-decodes the row on every frame */
	struct { size_t g, i; const uint8_t *b; SDL_Texture *t; int w, h; } tex[WHIMSY_HELD];
	/* one per key with an avatar. src is the core's pointer: a new one means new bytes.
	 * src set with t NULL is a remembered decode failure, same reason */
	struct { uint8_t key[WHIMSY_PK]; int grp; const uint8_t *src; SDL_Texture *t;
	         int w, h; uint64_t used; } av[AVATARS];
	uint64_t avclock;
	void *clipdata;                 /* the bytes SDL hands out for a file copy */
	size_t clipn;
	struct field comp;
	size_t cstart[MAXWRAP + 2];     /* composer line starts, from the last paint */
	int cnl, ctop;                  /* wrapped lines, first one shown */
	float comp_x, comp_y, comp_lh;  /* composer text origin, for a click to caret */
	int clicks;                     /* the click count the last press carried */
	uint64_t blink;                 /* ticks of the last edit: the caret is solid first */
	char err[128];

	int pop_sel;                    /* highlighted popup row */
	char ask[96];                   /* a question standing above the composer */
	char pastetmp[64];              /* the pasted image's temp copy, alive while it stands */
	char pend[256];                 /* the command line waiting on it */
	char ans[3][WHIMSY_MAX_VAL + 1];
	int nans;
	char info[INFO_MAX][160];       /* :set, :verify and :me write here; the popup shows it */
	int ninfo;
	char find[64];                  /* the live search needle, without its '/' */
	size_t fat;                     /* the match last jumped to, (size_t)-1 for none */
	int action;                     /* what the shell must do: UI_QUIT or UI_SERVER */

	float *rh;                      /* row heights, one per message index */
	size_t nrh;
	float cache_w;
	size_t cache_g;

	struct hit hit[256];
	int nhit;

	struct lspan lspan[LSPANS];
	int nlspan;
	size_t sa_row, sa_off, sb_row, sb_off;  /* log selection: anchor, then head */
	int lsel;

	int set_slot;                   /* the emoji slot being replaced on an S_EMOJI row */

	int strip_on;                   /* the hover strip, painted after every row */
	size_t strip_i;
	float strip_x, strip_y;          /* its right edge and text baseline top */

	uint64_t next_poll, next_dial;
	unsigned backoff;               /* seconds, 1 2 4 8 ... 32 */
	int neterr;                     /* last dial or poll failure, shown beside `offline` */
};


/* drop the answers a question collected; `:pass` leaves store passphrases in them */
void ans_clear(struct ui *u);
void arm(struct ui *u, int mode, size_t i);
void cancel(struct ui *u);
uint16_t chan_id(struct ui *u);
void circle(struct ui *u, float cx, float cy, float r, const uint8_t *key, int grp, const char *name, const uint8_t col[3]);
float composer(struct ui *u, float x, float y, float w, int paint);
void del_msg(struct ui *u, size_t i);
float draw_cut(struct ui *u, float x, float y, const char *s, size_t n, float maxw, const uint8_t rgb[3]);
void edit_msg(struct ui *u, size_t i);
void ui_err(struct ui *u, const char *fmt, ...);
int exec(struct ui *u, const char *line);
void file_overlay(struct ui *u, float W, float H);
void hit(struct ui *u, float x, float y, float w, float h, int kind, size_t a, size_t b);
/* the same, first in the list: what floats over a row must win the click */
void hit_top(struct ui *u, float x, float y, float w, float h, int kind, size_t a, size_t b);
void idcol(struct ui *u, const uint8_t pk[WHIMSY_PK], uint8_t out[3]);
void invalidate(struct ui *u);
int is_dm(struct ui *u, size_t g);
void log_pane(struct ui *u, float x, float y, float w, float h);
void log_sel_at(struct ui *u, float mx, float my, size_t *row, size_t *off);
int log_sel_copy(struct ui *u);
const char *mime_of(const char *name);
int img_mime(const char *name);
const char *mode_tag(const struct ui *u);
const uint8_t *peer(struct ui *u, size_t g);
float popup(struct ui *u, float x, float bottom, float w);
void profile_card(struct ui *u, float W, float H);
void remember(struct ui *u, const char *key, double v);
float row_height(struct ui *u, size_t i, float bw);
void row_title(struct ui *u, size_t g, char *out, size_t cap);
void select_group(struct ui *u, size_t g, size_t ch);
void settings(struct ui *u, float W, float H);
void crop_open(struct ui *u, const char *path, int grp);
void crop_close(struct ui *u);
void crop_paint(struct ui *u, float W, float H);
int  crop_event(struct ui *u, const SDL_Event *e, float scale);
SDL_Texture *tex_for(struct ui *u, size_t i, const uint8_t *b, size_t n, float maxw, float *w, float *h);

void paste_clean(struct ui *u);
void clip_bytes(struct ui *u, const void *b, size_t n, const char *mime);
void clip_set(const char *s, size_t n);
void complete(struct ui *u);
int ment_list(struct ui *u, char buf[][64], const char *cand[], const char **word, size_t *wn);
int ment_open(struct ui *u);
const char *notify_name(int lv);
int notify_next(int lv);
void comp_step_line(struct ui *u, int delta);
size_t comp_at(struct ui *u, float mx, float my);
void del_answer(struct ui *u, char k);
int done(struct ui *u, int e);
int open_dm(struct ui *u, const uint8_t pk[WHIMSY_PK]);
int row_pills(struct ui *u, size_t i, struct pill *out, int cap);
void run_line(struct ui *u);
void ui_send(struct ui *u);
void set_apply(struct ui *u, const char *val);
void set_edit(struct ui *u, int i);
void set_slot_put(struct ui *u, const char *emoji);
int set_kind(int i);
int set_on(struct ui *u, int i);
void set_suggest(struct ui *u);
void typed(struct ui *u);
int yes(const char *s);

enum { S_TEXT, S_BOOL, S_COLOUR, S_FONT, S_CYCLE, S_EMOJI };

void scroll_to(struct ui *u, size_t i);
void run_cmd(struct ui *u, const char *line);
int arg_list(struct ui *u, char buf[][64], const char *cand[], const char *help[],
             int cap, const char **word, size_t *wn);
int set_key(struct ui *u, const char *key, const char *val);
void clip(struct ui *u, float x, float y, float w, float h, float scale);

#endif
