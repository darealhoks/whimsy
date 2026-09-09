#include "ui_int.h"

#include "audio.h"

#include <stdlib.h>

#include "file.h"

/* ---- search ---- */

/* puts row i at the top of the viewport, or as close as the newest rows allow */
void scroll_to(struct ui *u, size_t i)
{
	uint16_t id = chan_id(u);
	struct whimsy_msg m;
	float sum = 0;
	for (size_t k = i; k < whimsy_msg_count(u->w, u->g); k++)
		if (whimsy_msg(u->w, u->g, k, &m) == WHIMSY_OK && m.channel == id)
			sum += row_height(u, k, u->cache_w);
	u->scroll = sum - u->log_h > 0 ? sum - u->log_h : 0;
}

/* the composer holds the search needle while it starts with '/'; a changed one starts
 * the walk over */
static void track(struct ui *u)
{
	char was[sizeof u->find];
	snprintf(was, sizeof was, "%s", u->find);
	if (u->comp.buf[0] == '/') snprintf(u->find, sizeof u->find, "%s", u->comp.buf + 1);
	else u->find[0] = 0;
	if (strcmp(was, u->find)) u->fat = (size_t)-1;
	u->ninfo = 0;                   /* the next keystroke dismisses what a command listed */
}

/* enter lands on the newest match and then walks back through older ones; shift+enter
 * walks the other way. both wrap. */
static void search_step(struct ui *u, int back)
{
	size_t hits[64];
	size_t n = whimsy_search(u->w, u->g, u->find, hits, 64), k = 0;
	if (n > 64) n = 64;                     /* snprintf-style: the total, not what fit */
	uint16_t id = chan_id(u);
	struct whimsy_msg m;
	for (size_t j = 0; j < n; j++)
		if (whimsy_msg(u->w, u->g, hits[j], &m) == WHIMSY_OK && m.channel == id)
			hits[k++] = hits[j];
	if (!(n = k)) { ui_err(u, "no match"); return; }

	size_t at = u->fat, best = (size_t)-1;
	if (back) {
		for (size_t j = 0; j < n && best == (size_t)-1; j++)
			if (at != (size_t)-1 && hits[j] > at) best = hits[j];
		if (best == (size_t)-1) best = hits[0];
	} else {
		for (size_t j = n; j-- > 0 && best == (size_t)-1; )
			if (at == (size_t)-1 || hits[j] < at) best = hits[j];
		if (best == (size_t)-1) best = hits[n - 1];
	}
	u->fat = best;
	scroll_to(u, best);
}

static void step_channel(struct ui *u, int delta)
{
	size_t n = whimsy_channel_count(u->w, u->g);
	if (n < 2) return;
	select_group(u, u->g, (u->chan + n + (size_t)delta) % n);
}

/* a command run by a click instead of by the composer */
void run_cmd(struct ui *u, const char *line)
{
	snprintf(u->pend, sizeof u->pend, "%s", line);
	ans_clear(u);
	u->ninfo = 0;
	if (!exec(u, u->pend)) { u->pend[0] = 0; ans_clear(u); }
}

/* ctrl+v with an image or a file url on the clipboard sends it instead of typing it.
 * 0 leaves the paste to the composer. the temp copy stays while a question stands */
static int paste_file(struct ui *u)
{
	static const char *const mimes[] = {"image/png", "image/jpeg"};
	static const char *const ext[] = {"png", "jpg"};
	char line[512];

	for (int k = 0; k < 2; k++) {
		char dir[] = "/tmp/whimsy-XXXXXX", tmp[64];
		size_t n;
		void *b;
		if (!SDL_HasClipboardData(mimes[k])) continue;
		if (!(b = SDL_GetClipboardData(mimes[k], &n))) continue;
		if (!mkdtemp(dir)) { SDL_free(b); return 0; }
		snprintf(tmp, sizeof tmp, "%s/pasted.%s", dir, ext[k]);
		if (SDL_SaveFile(tmp, b, n)) {
			snprintf(line, sizeof line, "file %s", tmp);
			run_cmd(u, line);
		}
		SDL_free(b);
		if (u->ask[0]) snprintf(u->pastetmp, sizeof u->pastetmp, "%s", tmp);
		else { remove(tmp); rmdir(dir); }
		return 1;
	}

	if (SDL_HasClipboardText()) {
		char *t = SDL_GetClipboardText();
		int url = t && !strncmp(t, "file://", 7);
		if (url) snprintf(line, sizeof line, "file %s", t + 7);
		SDL_free(t);
		if (url) {
			for (char *q = line; *q; q++) if (*q == '\n' || *q == '\r') *q = 0;
			run_cmd(u, line);
			return 1;
		}
	}
	return 0;
}

/* my newest row in this channel, for `up` on an empty composer */
static size_t my_last(struct ui *u)
{
	uint16_t id = chan_id(u);
	struct whimsy_msg m;
	for (size_t i = whimsy_msg_count(u->w, u->g); i-- > 0; )
		if (whimsy_msg(u->w, u->g, i, &m) == WHIMSY_OK && m.channel == id &&
		    m.mine) return i;
	return (size_t)-1;
}

static void take_hit(struct ui *u, const struct hit *h)
{
	uint8_t pk[WHIMSY_PK];
	struct whimsy_msg m;
	struct pill p[16];

	switch (h->kind) {
	case H_GROUP: select_group(u, h->a, 0); break;
	case H_CHAN:  select_group(u, h->a, h->b); break;
	case H_SIDE:  u->side_open = !u->side_open; remember(u, "side_open", u->side_open); break;
	case H_MEMB:  u->memb_open = !u->memb_open; remember(u, "memb_open", u->memb_open); break;
	case H_SDRAG: case H_MDRAG: u->drag = h->kind; break;
	case H_GOTO:  scroll_to(u, h->a); break;
	case H_UNARM: cancel(u); break;
	case H_COMP: {
		size_t i = comp_at(u, u->mx, u->my);
		u->blink = SDL_GetTicks();
		if (u->clicks >= 3) { u->comp.anc = 0; u->comp.cur = u->comp.n; break; }
		if (u->clicks == 2) { field_word_at(&u->comp, i); break; }
		u->comp.cur = u->comp.anc = i;
		u->drag = H_COMP;
		break;
	}
	case H_NEWPILL: u->scroll = 0; u->newat = (size_t)-1; invalidate(u); break;
	case H_ACT:
		u->act_i = h->a;
		if (whimsy_msg(u->w, u->g, h->a, &m) != WHIMSY_OK) break;
		if (h->b == A_REPLY) arm(u, M_REPLY, h->a);
		else if (h->b == A_EDIT) edit_msg(u, h->a);
		else if (h->b == A_DEL) del_msg(u, h->a);
		else if (h->b == A_SAVE) run_cmd(u, "save");
		else if (h->b == A_FCOPY) {
			size_t n;
			const uint8_t *b = whimsy_file_open(u->w, u->g, h->a, &n);
			char nm[256];
			const char *mime;
			if (!b) { ui_err(u, "no longer held"); break; }
			snprintf(nm, sizeof nm, "%.*s", (int)m.text_n, m.text);
			if ((mime = mime_of(nm))) clip_bytes(u, b, n, mime);
			else clip_set((const char *)b, n);
			ui_err(u, "copied to clipboard");
		}
		else {
			clip_set(m.text, m.text_n);
			ui_err(u, "copied to clipboard");
		}
		break;
	case H_AUDIO: {
		size_t n;
		const uint8_t *b;
		const char *ext, *err;
		u->act_i = h->a;
		if (whimsy_msg(u->w, u->g, h->a, &m) != WHIMSY_OK) break;
		if (h->b && audio_state(u->g, h->a, NULL)) {
			audio_seek(h->w > 0 ? (u->mx - h->x) / h->w : 0);
			break;
		}
		if (!(b = whimsy_file_open(u->w, u->g, h->a, &n))) { ui_err(u, "no longer held"); break; }
		if (!(ext = audio_ext(m.text, m.text_n))) break;
		if ((err = audio_toggle(u->g, h->a, ext, b, n))) ui_err(u, "%s", err);
		break;
	}
	case H_ZOOM:
		u->act_i = h->a;
		u->over = (int)h->b;
		u->over_i = h->a;
		break;
	case H_LINK: {
		char url[512];
		size_t j = h->b;
		if (whimsy_msg(u->w, u->g, h->a, &m) != WHIMSY_OK) break;
		while (j < m.text_n && m.text[j] != ' ' && m.text[j] != '\n') j++;
		while (j > h->b && strchr(".,;:!?)", m.text[j - 1])) j--;
		if (j - h->b >= sizeof url) break;
		snprintf(url, sizeof url, "%.*s", (int)(j - h->b), m.text + h->b);
		SDL_OpenURL(url);
		break;
	}
	case H_SPOIL: u->spoil = u->spoil == h->a ? (size_t)-1 : h->a; break;
	case H_RCT: {
		size_t n = 0;
		const char *em = react_nth(u->c->reacts, (int)h->b, &n);
		u->act_i = h->a;
		if (em && whimsy_msg(u->w, u->g, h->a, &m) == WHIMSY_OK)
			done(u, whimsy_react(u->w, u->g, m.id, em, n));
		break;
	}
	case H_PILL: {
		int np = row_pills(u, h->a, p, 16);
		u->act_i = h->a;
		if ((int)h->b >= np || whimsy_msg(u->w, u->g, h->a, &m) != WHIMSY_OK) break;
		done(u, p[h->b].mine ? whimsy_react(u->w, u->g, m.id, NULL, 0)
		                     : whimsy_react(u->w, u->g, m.id, p[h->b].text, p[h->b].text_n));
		break;
	}
	case H_PROF:
		if (h->b) {
			if (whimsy_msg(u->w, u->g, h->a, &m) != WHIMSY_OK) break;
			memcpy(u->card_pk, m.sender, WHIMSY_PK);
		} else {
			const uint8_t *mm = whimsy_member(u->w, u->g, h->a);
			if (!mm) break;
			memcpy(u->card_pk, mm, WHIMSY_PK);
		}
		u->card = 1;
		break;
	case H_CBTN:
		memcpy(pk, u->card_pk, WHIMSY_PK);
		if (h->a == 0) done(u, whimsy_verified(u->w, pk) ? whimsy_unverify(u->w, pk)
		                                                 : whimsy_verify(u->w, pk));
		else if (h->a == 1) { u->card = 0; open_dm(u, pk); }
		else done(u, whimsy_block(u->w, pk, !whimsy_blocked(u->w, pk)));
		break;
	case H_CARD: break;
	case H_SCAT:
		u->set_cat = (int)h->a;
		u->set_row = -1;
		u->set_scroll = 0;
		return;
	case H_SROW:
		if (set_kind((int)h->a) == S_CYCLE) {
			int lv = whimsy_notify_level(u->w, u->g);
			done(u, whimsy_notify(u->w, u->g, notify_next(lv)));
		} else if (set_kind((int)h->a) == S_BOOL) {
			u->set_row = (int)h->a;
			set_apply(u, set_on(u, (int)h->a) ? "0" : "1");
		} else set_edit(u, (int)h->a);
		break;
	case H_SLOT:
		u->set_row = (int)h->a;
		u->set_slot = (int)h->b;
		u->set_nfont = 0;
		break;
	case H_SFONT:
		if ((int)h->a < u->set_nfont) set_apply(u, u->set_font[h->a]);
		break;
	}
}

/* the strip, the pills and the jump pill sit on top of the row they belong to */
static int on_top(int kind)
{
	return kind == H_ACT || kind == H_RCT || kind == H_PILL || kind == H_GOTO ||
	       kind == H_PROF || kind == H_NEWPILL || kind == H_ZOOM ||
	       kind == H_LINK || kind == H_SPOIL || kind == H_AUDIO;
}

/* a user bind wins over the hardcoded keys; an empty command means the config unbound
   the key, so it does nothing at all. 0 leaves the key to the handling below */
static int try_bind(struct ui *u, SDL_Keycode k, SDL_Keymod m)
{
	char spec[80], key[24];
	const char *name = SDL_GetKeyName(k), *cmd;
	char *q;

	if (!u->c || !u->c->nbind || !name || !*name) return 0;
	snprintf(spec, sizeof spec, "%s%s%s%s", (m & SDL_KMOD_CTRL) ? "ctrl+" : "",
	         (m & SDL_KMOD_ALT) ? "alt+" : "", (m & SDL_KMOD_SHIFT) ? "shift+" : "", name);
	while ((q = strchr(spec, ' '))) memmove(q, q + 1, strlen(q));
	if (!conf_keyspec(spec, key, sizeof key)) return 0;
	if (!(cmd = conf_bind(u->c, key))) return 0;
	if (*cmd) run_cmd(u, cmd);
	return 1;
}

int ui_event(struct ui *u, const SDL_Event *e, float scale)
{
	if (u->cr_t) return crop_event(u, e, scale);
	switch (e->type) {
	case SDL_EVENT_TEXT_INPUT: {
		char first = e->text.text[0];
		u->err[0] = 0;
		u->blink = SDL_GetTicks();
		if (u->set_open) {
			if (u->set_row >= 0 && set_kind(u->set_row) == S_EMOJI) set_slot_put(u, e->text.text);
		else if (u->set_row >= 0) { field_insert(&u->set_f, e->text.text); set_suggest(u); }
			return 1;
		}
		if (u->mode == M_DEL) { del_answer(u, first | 0x20); return 1; }
		if (u->ask_typing && (first == 'y' || first == 'Y' || first == 'n' || first == 'N')) {
			whimsy_set(u->w, WHIMSY_TYPING_SEND, yes(e->text.text) ? "1" : "0");
			u->ask_typing = 0;
			u->ask[0] = 0;
			return 1;
		}
		field_insert(&u->comp, e->text.text);
		track(u);
		if (u->comp.buf[0] != ':' && u->comp.buf[0] != '/' && !u->ask[0]) typed(u);
		return 1;
	}
	case SDL_EVENT_MOUSE_WHEEL: {
		float k = SDL_GetWindowPixelDensity(SDL_GetRenderWindow(u->r)) / scale;
		if (u->set_open) {
			u->set_scroll -= e->wheel.y * draw_line_height(u->d) * 3;
			if (u->set_scroll < 0) u->set_scroll = 0;
			return 1;
		}
		float mx = e->wheel.mouse_x * k, my = e->wheel.mouse_y * k;
		for (int i = 0; i < u->nhit; i++) {
			struct hit *h = &u->hit[i];
			if (h->kind != H_COMP) continue;
			if (mx < h->x || mx >= h->x + h->w || my < h->y || my >= h->y + h->h) continue;
			u->ctop -= (int)e->wheel.y;
			return 1;
		}
		u->scroll += e->wheel.y * draw_line_height(u->d) * 3;
		if (u->scroll < 0) u->scroll = 0;
		return 1;
	}
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		float k = SDL_GetWindowPixelDensity(SDL_GetRenderWindow(u->r)) / scale;
		float mx = e->button.x * k, my = e->button.y * k;
		u->mx = mx;
		u->my = my;
		u->clicks = e->button.clicks;
		u->err[0] = 0;
		u->lsel = 0;
		if (u->over) { u->over = OV_NONE; return 1; }
		if (u->set_open) {
			for (int i = 0; i < u->nhit; i++) {
				struct hit *h = &u->hit[i];
				if (h->kind != H_SROW && h->kind != H_SFONT && h->kind != H_SCAT &&
				    h->kind != H_SLOT) continue;
				if (mx < h->x || mx >= h->x + h->w || my < h->y || my >= h->y + h->h) continue;
				take_hit(u, h);
				return 1;
			}
			u->set_row = -1;        /* a click off the rows drops the edit, esc closes */
			u->set_nfont = 0;
			return 1;
		}
		for (int pass = u->card ? 0 : 1; pass < 3; pass++) {
			for (int i = 0; i < u->nhit; i++) {
				struct hit *h = &u->hit[i];
				if (pass == 0 && h->kind != H_CBTN) continue;   /* buttons first: H_CARD lies under them */
				if (pass == 1 && u->card && h->kind != H_CARD) continue;
				if (pass == 1 && !u->card && !on_top(h->kind)) continue;
				if (mx < h->x || mx >= h->x + h->w || my < h->y || my >= h->y + h->h) continue;
				take_hit(u, h);
				return 1;
			}
			if (u->card && pass == 1) { u->card = 0; return 1; }   /* a click outside closes the card */
		}
		if (my >= u->log_y && my < u->log_y + u->log_h) {       /* bare log text: drag to select */
			log_sel_at(u, mx, my, &u->sa_row, &u->sa_off);
			u->sb_row = u->sa_row;
			u->sb_off = u->sa_off;
			u->lsel = u->sa_row != (size_t)-1;
			u->drag = H_SEL;
		}
		return 1;
	}
	case SDL_EVENT_DROP_FILE: {
		char line[512];
		if (!e->drop.data) return 0;
		snprintf(line, sizeof line, "file %s", e->drop.data);
		run_cmd(u, line);
		return 1;
	}
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (u->drag == H_COMP || u->drag == H_SEL) { u->drag = 0; return 1; }
		if (u->drag == H_SDRAG) remember(u, "side_w", u->side_w);
		else if (u->drag == H_MDRAG) remember(u, "memb_w", u->memb_w);
		u->drag = 0;
		return 0;
	case SDL_EVENT_MOUSE_MOTION: {
		float k = SDL_GetWindowPixelDensity(SDL_GetRenderWindow(u->r)) / scale;
		float mx = e->motion.x * k, my = e->motion.y * k;
		u->mx = mx;
		u->my = my;
		if (!u->drag) return my >= u->log_y && my < u->log_y + u->log_h;
		if (u->drag == H_COMP) { u->comp.cur = comp_at(u, mx, my); return 1; }
		if (u->drag == H_SEL) { log_sel_at(u, mx, my, &u->sb_row, &u->sb_off); return 1; }
		if (u->drag == H_SDRAG) u->side_w = mx;
		else u->memb_w = u->win_w - mx;
		return 1;
	}
	case SDL_EVENT_KEY_DOWN: {
		SDL_Keycode k = e->key.key;
		SDL_Keymod m = e->key.mod;
		u->blink = SDL_GetTicks();
		int pop = (u->comp.buf[0] == ':' && !u->ask[0]) || ment_open(u);
		u->err[0] = 0;
		if (!u->set_open && !u->ask[0] && !u->comp.n && try_bind(u, k, m)) return 1;
		if (u->set_open) {
			if (k == SDLK_ESCAPE) {
				if (u->set_row >= 0) u->set_row = -1;
				else u->set_open = 0;
				return 1;
			}
			if (u->set_row < 0) return 1;
			if (u->set_nfont && (k == SDLK_UP || k == SDLK_DOWN)) {
				u->set_fsel += k == SDLK_UP ? -1 : 1;
				if (u->set_fsel < 0) u->set_fsel = 0;
				if (u->set_fsel >= u->set_nfont) u->set_fsel = u->set_nfont - 1;
				return 1;
			}
			if (set_kind(u->set_row) == S_EMOJI) {
				if (k == SDLK_LEFT || k == SDLK_RIGHT) {
					u->set_slot += k == SDLK_LEFT ? -1 : 1;
					if (u->set_slot < 0) u->set_slot = 0;
					if (u->set_slot >= REACT_SLOTS) u->set_slot = REACT_SLOTS - 1;
				} else if (k == SDLK_V && (m & SDL_KMOD_CTRL)) {
					char *t = SDL_GetClipboardText();
					if (t) { set_slot_put(u, t); SDL_free(t); }
				}
				return 1;
			}
			if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
				set_apply(u, u->set_nfont ? u->set_font[u->set_fsel] : u->set_f.buf);
				return 1;
			}
			if (!field_key(&u->set_f, k, m, 1)) return 1;
			set_suggest(u);
			return 1;
		}
		if (k == SDLK_ESCAPE) {
			if (u->card) { u->card = 0; return 1; }
			if (u->lsel) { u->lsel = 0; return 1; }
			if (u->ask_typing) { u->ask_typing = 0; u->ask[0] = 0; return 1; }
			if (u->mode && !u->comp.n) { u->mode = M_NONE; return 1; }
			if (u->comp.n || u->ask[0] || u->ninfo) cancel(u);
			return 1;
		}
		if (k == SDLK_TAB && pop) {
			complete(u);
			return 1;
		}
		if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
			if (u->ask[0] || u->comp.buf[0] == ':') run_line(u);
			else if (u->comp.buf[0] == '/') search_step(u, (m & SDL_KMOD_SHIFT) != 0);
			else if (m & SDL_KMOD_SHIFT) field_insert(&u->comp, "\n");
			else ui_send(u);
			return 1;
		}
		if ((m & SDL_KMOD_CTRL) && k == SDLK_C && u->comp.cur == u->comp.anc &&
		    log_sel_copy(u)) { ui_err(u, "copied to clipboard"); return 1; }
		if ((m & SDL_KMOD_CTRL) && k == SDLK_V && paste_file(u)) return 1;
		if ((m & SDL_KMOD_CTRL) && k == SDLK_K) {        /* quick switcher: :go with its popup */
			memset(&u->comp, 0, sizeof u->comp);
			field_insert(&u->comp, ":go ");
			u->pop_sel = 0;
			track(u);
			return 1;
		}
			if ((m & SDL_KMOD_CTRL) && k == SDLK_F) {
			memset(&u->comp, 0, sizeof u->comp);
			field_insert(&u->comp, "/");
			track(u);
			return 1;
		}
		if (pop && (k == SDLK_UP || k == SDLK_DOWN)) {
			u->pop_sel += k == SDLK_UP ? -1 : 1;
			if (u->pop_sel < 0) u->pop_sel = 0;
			return 1;
		}
		if (!(m & SDL_KMOD_ALT) && k == SDLK_UP && !u->comp.n && !u->ask[0]) {
			size_t i = my_last(u);
			struct whimsy_msg lm;
			if (i != (size_t)-1 && whimsy_msg(u->w, u->g, i, &lm) == WHIMSY_OK) {
				char t[FIELD_MAX];
				arm(u, M_EDIT, i);
				snprintf(t, sizeof t, "%.*s", (int)lm.text_n, lm.text);
				field_insert(&u->comp, t);
			}
			return 1;
		}
		if (!(m & SDL_KMOD_ALT) && (k == SDLK_UP || k == SDLK_DOWN) && u->cnl > 1) {
			comp_step_line(u, k == SDLK_UP ? -1 : 1);
			return 1;
		}
		if ((m & SDL_KMOD_ALT) && (k == SDLK_UP || k == SDLK_DOWN)) {
			step_channel(u, k == SDLK_UP ? -1 : 1);
			return 1;
		}
		if (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN) {
			u->scroll += (k == SDLK_PAGEUP ? 1 : -1) * draw_line_height(u->d) * 10;
			if (u->scroll < 0) u->scroll = 0;
			return 1;
		}
		if ((k == SDLK_HOME || k == SDLK_END) && !(m & SDL_KMOD_CTRL) && u->cnl > 1) {
			int cl = 0;
			for (int j = 0; j < u->cnl; j++) if (u->comp.cur >= u->cstart[j]) cl = j;
			size_t e2 = u->cstart[cl + 1];
			while (e2 > u->cstart[cl] && u->comp.buf[e2 - 1] == '\n') e2--;
			u->comp.cur = k == SDLK_HOME ? u->cstart[cl] : e2;
			if (!(m & SDL_KMOD_SHIFT)) u->comp.anc = u->comp.cur;
			return 1;
		}
		if (k == SDLK_BACKSPACE && u->mode && !u->comp.cur) { u->mode = M_NONE; return 1; }
		{
			int took = field_key(&u->comp, k, m, u->ask[0] || u->comp.buf[0] == ':' || u->comp.buf[0] == '/');
			track(u);
			return took;
		}
	}
	}
	return 0;
}

