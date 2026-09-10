/*
 * The test that matters most: a program that includes the tree's own
 * sw/common/zeitlos.h and gets the same answers as GCC does.
 *
 * zeitlos.h is 500-odd lines of register map, an X-macro'd syscall
 * enum built by #include'ing syscalls.def, struct definitions, and
 * macros three deep. Nothing else in the tree exercises the
 * preprocessor as hard, and every app in sw/apps starts by including
 * it -- so a compiler that cannot read it cannot build anything,
 * however good its code generator is.
 *
 * -U__riscv is required, and is the honest boundary of Phase 1:
 * maskirq() in that header is a `static inline` wrapping a raw
 * picorv32 custom instruction, and zcc has no inline assembler. The
 * #else branch is taken instead. See docs/zcc.md, "The maskirq
 * problem", for why that is a Phase 2 item rather than a gap to paper
 * over.
 */

#include <stdint.h>
#include <stdbool.h>
#include "zeitlos.h"
#include "zsys.h"

struct probe { char a; int b; char c; short d; };

int main(void) {
    line("sizeof_zobj", (int)sizeof(z_obj_t));
    line("sizeof_zmsg", (int)sizeof(z_msg_t));

    /* the X-macro enum, at three points along it */
    line("Z_SYS_EXIT", (int)Z_SYS_EXIT);
    line("Z_SYS_FS_SIZE", (int)Z_SYS_FS_SIZE);
    line("Z_SYS_FS_TRUNCATE", (int)Z_SYS_FS_TRUNCATE);

    line("Z_NONE", (int)Z_NONE);
    line("Z_UINT32", (int)Z_UINT32);
    line("Z_BLOB", (int)Z_BLOB);

    /* struct layout has to match GCC's exactly, or nothing this
     * compiler produces can talk to anything the tree already built */
    line("probe_size", (int)sizeof(struct probe));

    {
        z_obj_t o;
        o.type = Z_UINT32;
        o.val.uint32 = 0xdeadbeefu;
        line("obj_type", (int)o.type);
        puts_("obj_val ");
        putu(o.val.uint32);
        putch('\n');
    }

    puts_("built against the real zeitlos.h\n");
    return 0;
}
