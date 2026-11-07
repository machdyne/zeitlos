#ifndef CONVERT_PACK_H
#define CONVERT_PACK_H
/*
 * Zeitlos -- convert: a folder into a zip, tar or tar.gz. The folder is
 * walked here (sw/common/zwalk.c, a step at a time), the archive written
 * by sw/common/formats (zf_pack). docs/convert.md, "Packing a folder".
 * Apart from convert.c so tests/pack_host.c can run it on the build
 * machine.
 */
#include "formats/zf.h"

// `dir` into `out`, as `to` (ZF_T_ZIP, ZF_T_TAR, ZF_T_TGZ). The entries
// are named from the folder's own name down ("photos/2024/a.jpg"), as
// zip -r and tar make them. Paths equal to one of `skip` (the archive
// being written, if it is inside the folder) are left out. zf_cancel_poll
// is asked between steps, zf_progress told the bytes done.
int pack_folder(const char *dir, zf_type_t to, zf_io_t *out, const char *const skip[2]);

// After pack_folder(): entries left out -- too deep (16 folders), a
// name too long, a folder that could not be read to its end.
extern uint32_t pack_skipped;

// Supplied by the caller: reading one file, as convert.c's zf_io_t.
bool pack_open_file(const char *path, uint32_t size, zf_io_t *io);
void pack_close_file(zf_io_t *io);

#endif
