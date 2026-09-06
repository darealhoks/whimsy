#include "cmd.h"

#include <string.h>

/* args and help are the popup's whole documentation, so they read as the user types */
const struct cmd cmd_table[] = {
	{"group",  "n|r|d|a <name>",     "new group, rename, leave, avatar", {CA_SUB, CA_GROUP, CA_TEXT}, 0},
	{"chan",   "n|r|d <name> [new]", "channel: new, rename, delete",    {CA_SUB, CA_CHAN, CA_TEXT}, 1},
	{"add",    "<pk>",               "add a member",                    {CA_PEER, 0, 0}, 1},
	{"kick",   "<pk|pet>",           "remove a member",                 {CA_PEER, 0, 0}, 1},
	{"dm",     "<pk|pet>",           "open or create the dm",           {CA_PEER, 0, 0}, 0},
	{"pet",    "<pk|pet> <name>",    "local petname, empty clears",     {CA_PEER, CA_TEXT, 0}, 0},
	{"verify", "[pk|pet]",           "fingerprints, or mark compared",  {CA_PEER, 0, 0}, 0},
	{"link",   "<pk>",               "your other device, run on both",  {CA_PEER, 0, 0}, 0},
	{"file",   "[path]",             "send a file, empty picks one",    {CA_PATH, 0, 0}, 0},
	{"avatar", "[path]",             "your avatar, empty clears it",    {CA_PATH, 0, 0}, 0},
	{"save",   "[path]",             "save the file, empty asks where", {CA_PATH, 0, 0}, 0},
	{"purge",  "<n> | all <n>",       "delete your last n; all: everyone's oldest n", {CA_SUB, 0, 0}, 0},
	{"block",  "<pk|pet>",           "hide what a key sends, again shows", {CA_PEER, 0, 0}, 0},
	{"mute",   "",                   "mute this group, again unmutes",  {0, 0, 0}, 0},
	{"me",     "",                   "your key and fingerprint",        {0, 0, 0}, 0},
	{"pass",   "",                   "change the store passphrase",     {0, 0, 0}, 0},
	{"server", "",                   "paste another invite",            {0, 0, 0}, 0},
	{"set",    "[key] [value]",      "settings; alone lists them",      {CA_KEY, CA_TEXT, 0}, 0},
	{"nuke",   "",                   "erase everything on this device",   {0, 0, 0}, 0},
	{"quit",   "",                   "close whimsy",                    {0, 0, 0}, 0},
};
const int cmd_count = (int)(sizeof cmd_table / sizeof *cmd_table);

void cmd_parse(const char *s, struct cmd_line *out)
{
	out->nw = 0;
	for (const char *p = s; *p && out->nw < CMD_MAXW; ) {
		while (*p == ' ') p++;
		if (!*p) break;
		const char *b = p;
		while (*p && *p != ' ') p++;
		out->w[out->nw] = b;
		out->n[out->nw] = (size_t)(p - b);
		out->nw++;
	}
}

const char *cmd_tail(const char *s, int i)
{
	while (*s == ' ') s++;
	for (; i > 0; i--) {
		while (*s && *s != ' ') s++;
		while (*s == ' ') s++;
	}
	return s;
}

const struct cmd *cmd_find(const char *name, size_t n)
{
	for (int i = 0; i < cmd_count; i++)
		if (strlen(cmd_table[i].name) == n && !memcmp(cmd_table[i].name, name, n))
			return &cmd_table[i];
	return NULL;
}

int cmd_match(const char *pfx, size_t pfxn, int owner, const struct cmd **out, int cap)
{
	int n = 0;
	for (int i = 0; i < cmd_count && n < cap; i++) {
		if (cmd_table[i].owner && !owner) continue;
		if (pfxn && (strlen(cmd_table[i].name) < pfxn ||
		             memcmp(cmd_table[i].name, pfx, pfxn))) continue;
		out[n++] = &cmd_table[i];
	}
	return n;
}

int cmd_complete(const char *word, size_t wn, const char *const *cand, int ncand,
                 char *out, size_t cap)
{
	const char *first = NULL;
	size_t common = 0;
	int n = 0;
	for (int i = 0; i < ncand; i++) {
		if (strlen(cand[i]) < wn || memcmp(cand[i], word, wn)) continue;
		if (!n++) { first = cand[i]; common = strlen(first); continue; }
		size_t k = 0;
		while (k < common && cand[i][k] && cand[i][k] == first[k]) k++;
		common = k;
	}
	if (!n) return 0;
	if (!cap) return n;
	if (common >= cap) common = cap - 1;
	memcpy(out, first, common);
	out[common] = 0;
	return n;
}
