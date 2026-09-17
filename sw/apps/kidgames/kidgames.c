/*
 * kidgames -- entry point.
 *
 * A compilation of educational games for kids, ported from
 * https://github.com/machdyne/kidgames (ncurses, Kakao Linux) to
 * Zeitlos. See docs/kidgames_app.md.
 *
 *     > run kidgames
 *
 * Starts in the 320x240 pixel-doubled viewport where the bitstream has
 * one, because 5x8 text at 1:1 is not readable from across a room by
 * someone who is still learning to read at all. F2 drops to a window.
 *
 * Structure is the original's, unchanged: initialise once, show the
 * menu, call the selected game's run() directly, loop back when it
 * returns. Escape at the menu quits. No fork/exec -- one process, one
 * allocation, one copy of the shared word lists and art.
 */

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zrng.h"

#include "kgui.h"
#include "kgpad.h"
#include "game.h"
#include "kgsound.h"

int main(void)
{
	const game_t *visible[32];
	const char *labels[32];
	int n;

	/*
	 * Seed before anything draws.
	 *
	 * The original calls srand(time(NULL)). There is no time() worth
	 * the name here -- the RTC is optional and may be unset -- and
	 * rand() would be picolibc's. z_rng_* is seeded from rtl/trng.v
	 * where the board has one and falls back to a ChaCha20 stream
	 * stirred from whatever entropy there is where it does not, so a
	 * board with no TRNG still deals a different word list every boot
	 * rather than the same one forever.
	 */
	z_rng_reseed();

	if (!kg_init()) return 1;

	/* After kg_init(), so a board with no audio has already printed
	 * whatever it has to say about game mode first and the two notes
	 * do not interleave. Silence is a normal outcome and says nothing
	 * -- see kgsound.h. */
	kg_sound_init();

	n = games_visible(visible, labels);

	for (;;) {

		int sel = kg_menu("KIDGAMES", labels, n);

		if (sel < 0) break;
		if (sel >= n) continue;

		visible[sel]->run();

		/* A game that replaced the repaint callback and forgot to put
		 * it back would leave the menu unable to redraw itself. Clear
		 * it here rather than trusting ten files to be tidy: kg_menu()
		 * installs its own immediately anyway, so this only has to
		 * cover the gap. */
		kg_set_repaint(0, 0);
		kg_pad_hide();

	}

	kg_sound_shutdown();
	kg_shutdown();

	return 0;
}
