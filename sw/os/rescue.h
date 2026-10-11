#ifndef Z_RESCUE_H
#define Z_RESCUE_H

/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The rescue console (docs/rescue_console.md): the kernel shell on the
 * monitor, typed at on the USB keyboard, with no wm. Hold ESC on the
 * keyboard while the machine boots; it also comes up by itself when wm
 * cannot be started or ends. The serial console works as always, and
 * both show the same prompt.
 *
 * Everything is in rescue.c. The rest of the kernel calls it at these
 * points only:
 *
 *   sh.c      rescue_boot_poll() in the boot window, rescue_enter() when
 *             ESC was held or wm could not be started
 *   kernel.c  rescue_exec() when a program is started (wm: hand over),
 *             rescue_ended() when one exits or crashes (wm: take over)
 *   uart.c    rescue_note() for each byte of console output,
 *             rescue_pump() to draw it, rescue_pending() while pid 0 waits
 *   hid.c     rescue_key() for each key event
 */

#include <stdbool.h>
#include <stdint.h>

// rescue_boot_poll()'s answers.
#define RESCUE_POLL_NO		0	// nothing: the window ends on time
#define RESCUE_POLL_ESC		1	// ESC is held on a keyboard: rescue
#define RESCUE_POLL_WAIT	2	// a USB device is still coming up:
								// keep the window open a little longer

// Is the rescue console on the screen? Read in uart.c's hot path.
extern volatile bool k_rescue_on;

// The boot window, every pass: ticks since it opened.
int rescue_boot_poll(uint32_t elapsed);

// Take the screen: from pid 0, or from an interrupt (drawing then waits
// for pid 0). `why` is printed on the console.
void rescue_enter(const char *why);

// A program was created by this name as pid / pid has ended.
void rescue_exec(const char *name, uint32_t pid);
void rescue_ended(uint32_t pid);

// Console output: one byte was logged (IRQs masked, any context).
void rescue_note(void);
// Draw what is new. Only pid 0, outside interrupts, does anything.
void rescue_pump(void);
// Is there output not drawn yet? (pid 0 waiting for input)
bool rescue_pending(void);

// A key event, packed as hid.c's HID_EVENT(). True if the rescue
// console took it (it is then not queued for apps).
bool rescue_key(uint32_t ev);

#endif
