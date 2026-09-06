#ifndef WHIMSY_CONF_H
#define WHIMSY_CONF_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* $XDG_CONFIG_HOME/whimsy/whimsy.conf, written in full with a comment per key on
 * first run; that file is the user docs. Reloaded live when its mtime moves. */

struct conf {
	char path[512];
	time_t mtime;

	char font[128], font_bold[128], font_italic[128], font_emoji[128];
	double size, line_height, alpha;
	uint8_t bg[3], fg[3], dim[3], accent[3], line[3], sel[3];
	double radius, pad, gap;
	char renderer[16];              /* auto | software */
	char close[8];                  /* quit | hide */
	/* the notification command; %g group, %c channel, %s sender, %t text, %a avatar path */
	char notify[256];
	double avatars;                 /* 0: no avatar is fetched, shown or sent */
	char reacts[128];               /* the hover strip's emoji, space separated */
	double confirm_delete;          /* 0: delete without asking */

	/* pane layout, written back by the gui as it is dragged and toggled */
	double side_w, memb_w, side_open, memb_open;
};

/* fills defaults, writes the default file when missing, then parses it */
void conf_load(struct conf *c, const char *path);
/* 1 when the file changed on disk and was reparsed */
int conf_reload(struct conf *c);

/* #rrggbb into out; out keeps its value when v does not parse */
void conf_colour(uint8_t out[3], const char *v);

/* the config keys, in the order the settings overlay lists them; NULL terminated */
extern const char *const conf_keys[];
/* 1 when key is a config key and value parsed; rewrites only the matching `key =`
   line in the file so its comments survive, appending the line if it is missing */
int conf_set(struct conf *c, const char *key, const char *value);
/* the current value of key as text, snprintf-style; 0 when key is unknown */
size_t conf_getstr(const struct conf *c, const char *key, char *out, size_t cap);

#endif
