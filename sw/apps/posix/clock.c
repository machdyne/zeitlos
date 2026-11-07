/*
 * posix -- the clock, for `date` (text.c): the RTC and
 * system.rtc.timezone. On the host, tests/host_fs.c answers instead.
 */
#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "../../common/zrtc.h"
#include "../../common/zcfg.h"
#include "posix.h"

bool px_clock_now(uint32_t *utc) {
	if (!z_rtc_available() || !z_rtc_valid()) return false;
	*utc = z_rtc_seconds();
	return true;
}

bool px_clock_zone(char *out, int cap) {
	return z_cfg_get("system.rtc.timezone", out, (size_t)cap) && out[0];
}
