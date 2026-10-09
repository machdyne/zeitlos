/*
 * What zlinkapp.c needs from the OS that its own headers would bring in
 * with code that only runs on the target: built with ZLINK_HOST_TEST,
 * zlinkapp.c includes this instead, and test_zlink_app.c supplies the
 * functions.
 */
#ifndef ZLINK_HOST_H
#define ZLINK_HOST_H

#include <stdint.h>
#include <stdbool.h>

#ifndef Z_TICK_HZ
#define Z_TICK_HZ 732u
#endif
#define PX_STDOUT_TAG "stdout"

void z_launch_arg_set(const char *arg);
bool z_launch_arg_take(char *out, int outlen);
void z_rng_bytes(void *buf, uint32_t len);

// zauth.h's, as far as zlinkapp.c uses them
#define Z_AUTH_OK            0
#define Z_AUTH_E_BAD        -1
#define Z_AUTH_E_WAIT       -2
#define Z_AUTH_HAS_PASSWORD  1u
typedef struct { uint32_t flags; } z_auth_status_t;
int z_auth_status(z_auth_status_t *st);
int z_auth_check(const char *pw, uint32_t len, uint32_t *wait_ms);

// How many upgrade answers ('u') the service "sends" without sending:
// the case of an answer lost on the wire. Shared by both machines (the
// test's shared memory): whichever is the secondary loses it.
extern volatile int *zl_test_lose_p;
#define zl_test_lose_answer (*zl_test_lose_p)

#endif
