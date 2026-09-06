#ifndef WHIMSY_UI_H
#define WHIMSY_UI_H

#include <SDL3/SDL.h>

#include "conf.h"
#include "draw.h"

/* The main screen. The only file that reads whimsy.h. */

struct ui;
struct whimsy;

int  ui_open(struct ui **out, struct whimsy *w, struct draw *d, struct conf *c, SDL_Renderer *r);
void ui_close(struct ui *u);
/* an event the shell did not take. 1 when something changed and a redraw is due */
int  ui_event(struct ui *u, const SDL_Event *e, float scale);
/* the 1s tick: poll when due, redial with backoff. 1 when a redraw is due */
int  ui_tick(struct ui *u);
void ui_paint(struct ui *u, float w, float h, float scale);

/* what a command asked the shell for, cleared by the read. :quit and :server are the
 * only two things the main screen cannot do itself */
enum { UI_NONE, UI_QUIT, UI_SERVER };
int ui_action(struct ui *u);

#endif
