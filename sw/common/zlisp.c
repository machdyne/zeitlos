/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * See zlisp.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zmsg.h"
#include "zlisp.h"

// a fresh tag per call, distinguishable from the 0 z_port_t's own
// CONNECT/CONNECTED exchange always uses -- not required for
// correctness (this function only ever has one request outstanding
// at a time, see zlisp.h's own header comment), just cheap insurance
// against ever confusing an EVAL reply for some unrelated message
// that happens to reuse tag 0.
static uint32_t next_tag = 1;

z_rv z_lisp_eval(uint32_t lisp_pid, const char *code,
	char *out, uint32_t out_cap, uint32_t timeout_ticks, bool *is_error) {

	uint32_t tag = next_tag++;

	z_msg_new_send(lisp_pid, Z_LISP_EVAL, tag, z_obj_str(code));

	uint32_t start = z_uptime_ticks();

	while ((z_uptime_ticks() - start) < timeout_ticks) {

		z_msg_t msg;
		if (z_msg_read(&msg) != Z_OK) continue;

		if (msg.tag != tag) continue; // not a reply to our request

		if (msg.subject == Z_LISP_RESULT || msg.subject == Z_LISP_ERROR) {

			*is_error = (msg.subject == Z_LISP_ERROR);

			if (msg.obj.type == Z_STR && msg.obj.val.str) {
				snprintf(out, out_cap, "%s", msg.obj.val.str);
			} else {
				out[0] = 0;
			}

			return Z_OK;

		}

		// not a reply to our request -- discard and keep waiting,
		// same as z_port_connect()/z_msg_wait() do for any RPC-style
		// exchange

	}

	return Z_FAIL; // timed out -- lisp likely isn't running

}
