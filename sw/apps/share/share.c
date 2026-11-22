/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * share -- the SD card as a drive on the PC, over the USB-C socket
 * (`USB_DEV, docs/usb_dev.md, "The card on the PC").
 *
 *   > share           share the card; give it back with Eject, or by
 *                     ejecting it on the PC
 *   > share           (again, while shared) give it back
 *
 * What happens, in order:
 *
 *   1. `fs` lets go of the card (zfs_umount("/")). It refuses while any
 *      file or folder on it is open (-Z_EBUSY): nothing is shared then.
 *      From here until step 6 nothing in Zeitlos reads or writes the
 *      card, so the PC can have it whole.
 *   2. The card's SPI driver here (sw/apps/fs/fatfs/sdmm.c, the one fs
 *      uses) starts the card again and reads its size.
 *   3. `usbd` steps aside (Z_USBD_PAUSE, zusbd.h): it leaves the bus and
 *      IRQ 11 and says how far its terminal had read the console log.
 *   4. This joins the bus as the same device with a card reader added:
 *      usbd.c built with -DUSBD_MSC. The PC sees the console and the
 *      network come back, and a drive.
 *   5. Until the PC ejects the card (START STOP UNIT), or Eject is
 *      pressed here, or `share` is run again (Z_USBD_EJECT): the card
 *      is the PC's. The screen says so, in a window every key goes to.
 *   6. Off the bus, IRQ 11 and the device back to `usbd`
 *      (Z_USBD_RESUME), and `fs` mounts the card again (zfs_mount("/")),
 *      reading whatever the PC left on it.
 *
 * Eject here only once the PC has finished writing: a PC that has not
 * been told (eject or "safely remove" there) may still hold writes it
 * has not sent. Ejecting on the PC first is always safe.
 *
 * With no window manager it runs on the console alone: what it is doing
 * goes to the console, and `share` run again gives the card back.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zusbd.h"
#include "xlcard.h"
#include "../../common/zfsc.h"
#include "../../common/zfsproto.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zwidget.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"

/* integers only: newlib's printf without floating point, a third of this program */
#define printf iprintf

#include "ff.h"
#include "diskio.h"

/* usbd.c, built here with -DUSBD_MSC -DUSBD_NO_MAIN */
uint32_t usbd_poll(void);
void usbd_detach(void);
void usbd_attach(uint32_t kp);
uint32_t usbd_console_pos(void);
int usbd_state(void);
void usbd_msc_medium(int on);
int usbd_msc_ejected(void);

/* sw/apps/fs/fatfs/sdmm.c */
DSTATUS sd_disk_initialize(BYTE drv);
DRESULT sd_disk_read(BYTE drv, BYTE *buff, LBA_t sector, UINT count);
DRESULT sd_disk_write(BYTE drv, const BYTE *buff, LBA_t sector, UINT count);
DRESULT sd_disk_ioctl(BYTE drv, BYTE ctrl, void *buff);

/* -- the card, for usbd.c -- */

static uint32_t card_sectors;

uint32_t usbd_card_sectors(void)
{
	return card_sectors;
}

int usbd_card_read(uint32_t lba, uint8_t *buf, uint32_t n)
{
	return sd_disk_read(0, buf, lba, n) != RES_OK;
}

int usbd_card_write(uint32_t lba, const uint8_t *buf, uint32_t n)
{
	return sd_disk_write(0, buf, lba, n) != RES_OK;
}

/* -- the window -- */

#define WIN_W	260
#define WIN_H	96
#define LINE_H	11

static z_win_t win;
static bool have_win;
static bool sharing;			/* the window is the shared screen, with a status line */
static z_widget_t btn;
static z_widget_set_t wset;
static int shown_state = -1;		/* what the status line last said */

static const char *const share_lines[3] = {
	"The SD card is shared with the PC.",
	"Eject it on the PC when you are done,",
	"or press Eject once the PC has written.",
};
static const char *lines[3];

/* the card over the XA-XD link (Sergei, Mozart ML2: xlcard.c), not the
   USB device on a USB-C socket */
static bool by_link;

static int pc_has_it(void)
{
	return by_link ? xlc_pc() : (usbd_state() & 1);
}

static const char *status_text(void)
{
	return pc_has_it() ? "The PC has the card." : "Waiting for the PC...";
}

static void draw_status(void)
{
	int y = 4 + 3 * LINE_H + 4;
	if (!sharing)
		return;
	z_win_fill_rect(&win, 0, y, z_win_content_w(&win), z_font_5x8.h, 0);
	z_win_draw_text(&win, 8, y, status_text(), 1, &z_font_5x8);
	shown_state = pc_has_it();
}

static void repaint(void)
{
	int i;
	z_win_clear(&win);
	for (i = 0; i < 3; i++)
		if (lines[i])
			z_win_draw_text(&win, 8, 4 + i * LINE_H, lines[i], 1, &z_font_5x8);
	draw_status();
	z_widget_draw_all(&wset, true);
}

/* the window: three lines of text and one button */
static bool win_open(const char *title, const char *const l[3], const char *label)
{
	memcpy(lines, l, sizeof(lines));
	if (z_win_create_flags(&win, title, WIN_W, WIN_H, -1, -1,
			Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_MODAL) != Z_OK)
		return false;
	memset(&btn, 0, sizeof(btn));
	btn.type = Z_WIDGET_BUTTON;
	btn.label = label;
	btn.enabled = true;
	btn.w = 64;
	btn.h = 18;
	btn.x = (int16_t)((z_win_content_w(&win) - btn.w) / 2);
	btn.y = (int16_t)(z_win_content_h(&win) - btn.h - 6);
	z_widget_set_init(&wset, &btn, 1, &win);
	z_widget_focus_set(&wset, 0);
	repaint();
	return true;
}

/* one message for the window: true if it asks for the card back */
static bool win_msg(z_msg_t *m)
{
	int cx, cy;

	switch (m->subject) {
	case Z_WM_SET_CLIP:
		z_win_apply_clip(&win, &m->obj);
		break;
	case Z_WM_REDRAW:
		if (m->obj.type != Z_UINT32 || z_win_redraw_id(m->obj.val.uint32) != win.id)
			break;
		z_win_apply_redraw(&win, m->obj.val.uint32);
		repaint();
		z_win_redraw_done(&win);
		break;
	case Z_WM_WINDOW_MOVED:
		z_win_parse_rect(&win, &m->obj);
		break;
	case Z_WM_CLOSE:
		return true;
	case Z_WM_MOUSE:
		if (m->obj.type != Z_UINT32)
			break;
		if (!z_win_mouse_content_xy(&win, m->obj.val.uint32, &cx, &cy) && wset.pressed < 0)
			break;
		if (z_widget_mouse(&wset, cx, cy,
				(uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(m->obj.val.uint32)) == 0)
			return true;
		break;
	case Z_WM_KEY:
		if (m->obj.type != Z_UINT32 || !Z_WM_UNPACK_KEY_PRESSED(m->obj.val.uint32))
			break;
		switch (Z_WM_UNPACK_KEY_KEYSYM(m->obj.val.uint32)) {
		case 0x0d:		/* Enter */
		case ' ':
			return true;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return false;
}

/* -- telling the user -- */

/* Why the card was not shared: on the console, and, with wm, in a box
 * until OK. s is the console's line; l1 and l2 the box's. */
static int fail(const char *s, const char *l1, const char *l2)
{
	const char *l[3];
	z_msg_t m;

	printf("share: %s\n", s);
	l[0] = "The SD card is not shared:";
	l[1] = l1;
	l[2] = l2;
	if (!win_open("Card not shared", l, "OK"))
		return 1;
	for (;;) {
		if (z_msg_recv(&m, 0) == Z_OK && win_msg(&m))
			break;
	}
	z_win_destroy(&win);
	return 1;
}

/* -- handing the device over -- */

static uint32_t usbd_pid;
static bool usbd_paused;
static bool irq;

/* usbd off the device: the console log position its terminal had */
static bool take_device(uint32_t *kp)
{
	z_obj_t o;
	z_msg_t m;

	*kp = 0;
	if (!z_pid_lookup("usbd0", &usbd_pid)) {
		usbd_pid = 0;		/* not running: the device is nobody's */
		usbd_detach();
		return true;
	}
	memset(&o, 0, sizeof(o));
	o.type = Z_NONE;
	if (z_msg_new_send(usbd_pid, Z_USBD_PAUSE, 0, o) != Z_OK)
		return false;
	if (z_msg_recv_match(&m, Z_USBD_PAUSED, 0, 2 * Z_TICK_HZ) != Z_OK)
		return false;
	if (m.obj.type != Z_UINT32)
		return false;		/* refused */
	usbd_paused = true;
	*kp = m.obj.val.uint32;
	return true;
}

static void give_device(void)
{
	z_obj_t o;

	usbd_detach();
	if (irq)
		(void)z_irq_unbind(Z_IRQ_USBD);
	irq = false;
	if (!usbd_paused)
		return;
	memset(&o, 0, sizeof(o));
	o.type = Z_UINT32;
	o.val.uint32 = usbd_console_pos();
	(void)z_msg_new_send(usbd_pid, Z_USBD_RESUME, 0, o);
	usbd_paused = false;
}

/* -- the link instead (ML2): netx steps aside, not usbd -- */

static uint32_t netx_pid;
static bool netx_paused;

/* the RP2040 is on the link: it polls, so the frame count moves */
static bool rp2040_there(void)
{
	uint32_t f = xlc_frames(), t0 = z_uptime_ticks();
	while (z_uptime_ticks() - t0 < Z_TICK_HZ / 2) {
		if (xlc_frames() != f)
			return true;
		z_proc_wait(Z_TICK_HZ / 50 + 1);
	}
	return false;
}

static bool take_link(void)
{
	z_obj_t o;
	z_msg_t m;
	if (!z_pid_lookup("netx0", &netx_pid)) {
		netx_pid = 0;		/* no netx: the link is nobody's */
		return true;
	}
	memset(&o, 0, sizeof(o));
	o.type = Z_NONE;
	if (z_msg_new_send(netx_pid, Z_USBD_LINK_PAUSE, 0, o) != Z_OK)
		return false;
	if (z_msg_recv_match(&m, Z_USBD_LINK_PAUSED, 0, 2 * Z_TICK_HZ) != Z_OK)
		return false;
	if (m.obj.type != Z_UINT32 || m.obj.val.uint32 != 1)
		return false;
	netx_paused = true;
	return true;
}

static void give_link(void)
{
	z_obj_t o;
	if (!netx_paused)
		return;
	memset(&o, 0, sizeof(o));
	o.type = Z_NONE;
	(void)z_msg_new_send(netx_pid, Z_USBD_LINK_RESUME, 0, o);
	netx_paused = false;
}

static void card_back(void)
{
	int32_t rc = zfs_mount("/");
	if (rc < 0)
		printf("share: the card is not mounted again (error %ld); `mount` tries again\n", (long)rc);
	else
		printf("share: the card is back\n");
}

/* steps 3 to 6 over the link (xlcard.c): the RP2040's card reader asks,
   this answers, until the PC ejects or Eject is pressed */
static int share_by_link(void)
{
	bool done = false, by_pc = false;
	uint32_t i, rd, wr;

	if (!take_link()) {
		card_back();
		return fail("netx would not let go of the link", "the link is busy.", 0);
	}
	xlc_begin(card_sectors, netx_pid != 0);
	printf("share: the SD card (%lu MB) is shared with the PC on the carrier's USB\n",
		(unsigned long)(card_sectors / 2048));
	sharing = true;
	have_win = win_open("Card shared", share_lines, "Eject");
	if (!have_win)
		printf("share: eject it on the PC, or run `share` again, to give it back\n");

	while (!done) {
		z_msg_t m;
		int r = xlc_serve();
		if (r & 2) {
			by_pc = true;
			break;
		}
		if (have_win && pc_has_it() != shown_state)
			draw_status();
		// a request just now: another is coming, so look again at once
		// (messages without waiting); otherwise wait a tick for one
		if (r & 1) {
			if (z_msg_read(&m) != Z_OK)
				continue;
		} else if (z_msg_recv(&m, 1) != Z_OK) {
			continue;
		}
		do {
			if (m.subject == Z_USBD_EJECT)
				done = true;
			else if (have_win && win_msg(&m))
				done = true;
		} while (!done && z_msg_read(&m) == Z_OK);
	}

	// not shared any more (FLAGS: the RP2040 tells the PC the medium is
	// gone); a moment answering "not shared" to what is still on its way
	xlc_end();
	for (i = 0; i < 10; i++) {
		(void)xlc_serve();
		z_proc_wait(Z_TICK_HZ / 100 + 1);
	}
	give_link();
	if (have_win)
		z_win_destroy(&win);
	xlc_counts(&rd, &wr);
	printf("share: %s (%lu KB read, %lu KB written)\n", by_pc ? "the PC ejected the card" : "card ejected",
		(unsigned long)(rd / 2), (unsigned long)(wr / 2));
	card_back();
	return 0;
}

int main(void)
{
	char name[16];
	uint32_t pid, kp;
	int32_t rc;
	DWORD n = 0;
	bool done = false, by_pc = false;

	// Already sharing: this run is the "give it back".
	if (z_pid_lookup(Z_USBD_SHARE_NAME, &pid)) {
		z_obj_t o;
		memset(&o, 0, sizeof(o));
		o.type = Z_NONE;
		(void)z_msg_new_send(pid, Z_USBD_EJECT, 0, o);
		printf("share: giving the card back\n");
		return 0;
	}
	if (!z_usb_dev() || zu_rd(ZU_MAGIC) != ZU_MAGIC_VAL) {
		// Sergei and Mozart ML2: the carrier's USB, through its RP2040
		if (!z_xlink() || !xlc_present())
			return fail("this bitstream has no USB device on the USB-C socket (USB_DEV)"
				" and no link to the carrier's USB (XLINK)",
				"this system has no USB device", "for the card.");
		if (!rp2040_there())
			return fail("the carrier's RP2040 is not on the link: start Zeitlos from"
				" machboot's menu (an ML2 menu), with netx",
				"the carrier's USB is not connected.", "Start Zeitlos from machboot's menu.");
		by_link = true;
	}
	if (!z_pid_register("share", name, sizeof(name)) || strcmp(name, Z_USBD_SHARE_NAME) != 0)
		return fail("cannot register", "it cannot register.", 0);

	// 1. fs lets go
	rc = zfs_umount("/");
	if (rc == -Z_EBUSY)
		return fail("files or folders on the card are open: close them first",
			"files or folders on it are open.", "Close them, then share again.");
	if (rc < 0)
		return fail("the card cannot be let go of", "the system cannot let go of it.", 0);

	// 2. the card, here
	if ((sd_disk_initialize(0) & STA_NOINIT) ||
			sd_disk_ioctl(0, GET_SECTOR_COUNT, &n) != RES_OK || n == 0) {
		card_back();
		return fail("the card does not answer", "the card does not answer.", 0);
	}
	card_sectors = (uint32_t)n;

	if (by_link)
		return share_by_link();

	// 3. usbd steps aside
	if (!take_device(&kp)) {
		card_back();
		return fail("usbd would not let go of the USB device",
			"the USB device is busy.", 0);
	}

	// 4. the device with the card reader
	irq = z_irq_bind(Z_IRQ_USBD, ZU_IRQEN, 0) == Z_OK;
	z_proc_wait(Z_TICK_HZ / 50 + 1);	/* the PC must see the old device go */
	usbd_msc_medium(1);
	usbd_attach(kp);
	printf("share: the SD card (%lu MB) is shared with the PC on the USB-C port\n",
		(unsigned long)(card_sectors / 2048));

	sharing = true;
	have_win = win_open("Card shared", share_lines, "Eject");
	if (!have_win)
		printf("share: eject it on the PC, or run `share` again, to give it back\n");

	// 5. the PC's, until one side gives it back
	while (!done) {
		z_msg_t m;
		uint32_t want, t;

		want = usbd_poll();
		if (irq)
			zu_wr(ZU_IRQEN, want);
		if (usbd_msc_ejected()) {
			done = true;
			by_pc = true;
			break;
		}
		if (have_win && (usbd_state() & 1) != shown_state)
			draw_status();
		// the console log, as usbd does: a hundred times a second while
		// a terminal is open
		t = (usbd_state() & 2) ? Z_TICK_HZ / 100 + 1 : Z_TICK_HZ / 4;
		if (!irq)
			t = 1;
		if (z_msg_recv(&m, t) != Z_OK)
			continue;
		do {
			if (m.subject == Z_USBD_EJECT)
				done = true;
			else if (have_win && win_msg(&m))
				done = true;
		} while (!done && z_msg_read(&m) == Z_OK);
	}

	// 6. back to usbd and fs
	if (!by_pc) {
		// The PC learns the card is gone (NOT READY) before the device
		// itself goes: a moment of serving with the medium off.
		uint32_t i;
		usbd_msc_medium(0);
		for (i = 0; i < 10; i++) {
			(void)usbd_poll();
			z_proc_wait(Z_TICK_HZ / 100 + 1);
		}
	}
	give_device();
	if (have_win)
		z_win_destroy(&win);
	printf("share: %s\n", by_pc ? "the PC ejected the card" : "card ejected");
	card_back();
	return 0;
}
