/*
 * The headers a colour program needs: zsoc.h and zwm.h, through libz.
 *
 * Two things in them stopped zcc, and this keeps them from coming back:
 *
 *   - zsoc.h declares its feature tables `extern const T name[];`, an
 *     array with no size. zcc refused that at file scope ("needs a
 *     size"), though it is ordinary C for something defined elsewhere.
 *   - zwm.h reaches zsubjects.h, whose subject-block checks are
 *     _Static_assert, which zcc does not parse. zsubjects.h maps them
 *     away under __zcc__ (GCC still checks them).
 *
 * sw/data/demo/mandel.c (docs/demo.md) is the program that found both.
 */

#include "libz.h"
#include "zsoc.h"
#include "zwm.h"

extern const int unsized_table[];       /* declared, never defined */

int main(void) {
    printf("grab %d release %d revoked %d\n", (int)Z_WM_GAME_GRAB,
           (int)Z_WM_GAME_RELEASE, (int)Z_WM_GAME_REVOKED);
    printf("offsets %u\n", Z_COLOR_OFFSETS(Z_COLOR_DX, Z_COLOR_DY, 3));
    printf("feature info %d\n", (int)sizeof(z_feature_info_t));
    return 0;
}
