#ifndef WHIMSY_CMD_H
#define WHIMSY_CMD_H

#include <stddef.h>

/* The `:` command table and the pure half of the line editing around it: splitting,
 * filtering for the popup, completing a word. No SDL, no whimsy.h -- ui.c runs the
 * commands, this only says what they are and how a line breaks up. */

#define CMD_MAXW 6              /* words a command line is split into */

/* what the popup completes an argument from */
enum cmd_arg { CA_NONE, CA_SUB, CA_CHAN, CA_GROUP, CA_PEER, CA_KEY, CA_EMOJI, CA_PATH, CA_TEXT, CA_NOTIFY, CA_GOTO };

struct cmd {
	const char *name;
	const char *args;               /* shown in the popup, not parsed */
	const char *help;
	unsigned char type[3];
	unsigned char owner;            /* hidden from the popup for a non-owner */
};

extern const struct cmd cmd_table[];
extern const int cmd_count;

struct cmd_line {
	const char *w[CMD_MAXW];
	size_t n[CMD_MAXW];
	int nw;
};

/* splits on runs of spaces; s is the line without its leading ':' */
void cmd_parse(const char *s, struct cmd_line *out);
/* the line from word i to the end, spaces kept; "" past the last word */
const char *cmd_tail(const char *s, int i);
const struct cmd *cmd_find(const char *name, size_t n);
/* commands whose name starts with pfx, owner-only ones left out unless owner */
int cmd_match(const char *pfx, size_t pfxn, int owner, const struct cmd **out, int cap);
/* longest common prefix of the candidates that start with word, written to out.
 * returns how many candidates matched; out is untouched when none did */
int cmd_complete(const char *word, size_t wn, const char *const *cand, int ncand,
                 char *out, size_t cap);

#endif
