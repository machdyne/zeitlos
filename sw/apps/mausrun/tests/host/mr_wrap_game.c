/*
 * mausrun.c itself, for the host: its frame waits go to the host's
 * frame boundary (mr_host.c), and its main() is called by render.c.
 * zsoc.h is read first so the rename applies to the calls only.
 */
#include "../../../../common/zsoc.h"
void host_wait_frame(void);
#define z_game_wait_frame host_wait_frame
#define main game_main
#include "../../mausrun.c"

/* for render.c's hook: the game's state, read-only */
mr_actor_t mr_host_player(void) { return player; }
int mr_host_state(void) { return (int)st; }
int mr_host_level(void) { return level_no; }
int mr_host_lives(void) { return lives; }

/* a foe that hurts in front of the mouse at about its height: 2 if
 * it is close, 0 for none */
int mr_host_cat_ahead(int32_t x, int32_t y) {
	int i;
	for (i = 0; i < foe_count; i++) {
		int32_t cx = TOPX(foes[i].a.x), cy = TOPX(foes[i].a.y);
		if (!foes[i].a.alive || !foe_harmful(&foes[i]) || cy - y > 24 || y - cy > 24) continue;
		if (cx - (x + MR_PLAYER_W) > 8 && cx - (x + MR_PLAYER_W) < 20) return 2;
	}
	return 0;
}

bool mr_host_cat_near(int32_t x, int32_t y) {
	int i;
	for (i = 0; i < foe_count; i++) {
		int32_t cx = TOPX(foes[i].a.x), cy = TOPX(foes[i].a.y);
		if (!foes[i].a.alive || !foe_harmful(&foes[i]) || cy - y > 24 || y - cy > 24) continue;
		if (cx - (x + MR_PLAYER_W) >= 0 && cx - (x + MR_PLAYER_W) < 44) return true;
	}
	return false;
}

/* round 3: for the script */
int mr_host_cheese(void) { return cheese; }
int mr_host_hurt(void) { return hurt_frames; }
int mr_host_power(int i) { return i < PW_TIMED ? pw_left[i] : pw_bubble; }
int mr_host_parts(void) {
	int i, n = 0;
	for (i = 0; i < MR_MAX_PARTS; i++) n += parts[i].kind != P_NONE;
	return n;
}
int mr_host_foe_state(int i) { return i < foe_count ? foes[i].state : -1; }
void mr_host_give(int pw) { if (pw < PW_TIMED) pw_left[pw] = power_frames[skill]; else pw_bubble = true; }

/* the mouse put in the air over the first cat that is walking on
 * screen: it comes down on its head */
bool mr_host_drop_on_cat(void) {
	int i;
	for (i = 0; i < foe_count; i++) {
		mr_foe_t *f = &foes[i];
		if (f->kind != MR_FOE_CAT || f->state != FS_WALK || !f->a.alive) continue;
		if (!on_screen(TOPX(f->a.x))) continue;
		player.x = f->a.x + TOFP(3);
		player.y = f->a.y - TOFP(40);
		player.vy = 0;
		player.on_ground = false;
		return true;
	}
	return false;
}

/* round 4: straight to a level, in play */
void mr_host_goto(int n) {
	lives = lives_for[skill];
	intro(n);
	set_state(ST_PLAY);
}

/* the mouse dropped onto the first plank: then where it is, and
 * whether it is riding */
bool mr_host_drop_on_plank(void) {
	if (!plat_count) return false;
	player.x = plats[0].x + TOFP(10);
	player.y = TOFP(plats[0].y - MR_PLAYER_H - 20);
	player.vy = 0;
	player.on_ground = false;
	return true;
}
int mr_host_riding(void) { return riding; }
int32_t mr_host_plank_x(void) { return plat_count ? TOPX(plats[0].x) : -1; }
int mr_host_theme(void) { return mr_level_info.theme; }

/* the blits drawn this frame so far (read at the frame wait, before the
 * main loop resets the count) */
uint32_t mr_host_blits(void) { return hud_blits; }
bool mr_host_colour(void) { return col_on; }
