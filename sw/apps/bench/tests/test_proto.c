/*
 * bench -- host tests for the protocol's encoding (zbench.h).
 */

#include <stdio.h>
#include <string.h>
#include "../../../common/zbench.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

int main(void) {
    uint8_t buf[ZB_NAME + ZB_MAX + 8], w[3] = { 6, 0, 0xFF };
    const char *bus;
    const uint8_t *wp;
    uint8_t addr;
    int wn, rn;
    int n = zb_xfer_encode(buf, "main", 0x20, w, 3, 2);
    CHECK(n == 1 + 5 + 2 + 3 + 1, "length %d", n);
    CHECK(zb_xfer_decode(buf, n, &bus, &addr, &wp, &wn, &rn) == 0 && !strcmp(bus, "main") &&
          addr == 0x20 && wn == 3 && !memcmp(wp, w, 3) && rn == 2, "round trip");
    n = zb_xfer_encode(buf, "main", 0x20, 0, 0, 0);
    CHECK(zb_xfer_decode(buf, n, &bus, &addr, &wp, &wn, &rn) == 0 && wn == 0 && rn == 0, "a probe");
    CHECK(zb_xfer_encode(buf, "a-bus-name-too-long", 1, 0, 0, 0) == -1, "a long name refused");
    CHECK(zb_xfer_encode(buf, "main", 1, w, ZB_MAX + 1, 0) == -1, "too many bytes refused");
    n = zb_xfer_encode(buf, "main", 0x20, w, 3, 2);
    CHECK(zb_xfer_decode(buf, n - 1, &bus, &addr, &wp, &wn, &rn) == -1, "short: refused");
    CHECK(zb_xfer_decode(buf, n + 1, &bus, &addr, &wp, &wn, &rn) == -1, "long: refused");
    buf[1] = 0;
    CHECK(zb_xfer_decode(buf, n, &bus, &addr, &wp, &wn, &rn) == -1, "no name: refused");
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all bench protocol tests passed\n");
    return 0;
}
