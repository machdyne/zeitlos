#ifndef ZOBJ_H
#define ZOBJ_H

#include <stdint.h>

typedef enum {
    Z_NONE,
    Z_RETVAL,
    Z_UINT32,
    Z_INT32,
    Z_FLOAT32,
    Z_STR,
    Z_LIST,
    Z_MAP,
    Z_BLOB    // arbitrary-length binary data, explicit length -- NOT
              // NUL-terminated like Z_STR, safe for data that may
              // contain zero bytes anywhere (e.g. network packets)
} z_type_t;

typedef struct {
    z_type_t type;
    union {
        void *ptr;          // used for lists, maps, blobs
        char *str;          // used for strings
        uint32_t uint32;    // used for unsigned integers
        int32_t int32;      // used for signed integers
        float float32;      // used for floats
    } val;
} z_obj_t;

// used by lists, maps
typedef struct {
    uint32_t len;
    z_obj_t *a;
    z_obj_t *b;
} z_obj_table_t;

// used by blobs (Z_BLOB) -- val.ptr points to one of these
typedef struct {
    uint32_t len;
    uint8_t *data;
} z_blob_t;

// Return value objects -- the shared &z_ok/&z_fail every syscall
// handler returns (see sw/os/fsapi.h's own header comment on that
// convention).
//
// DECLARED here, DEFINED once in zobj.c. These used to be `static`
// definitions right here in the header, which gave every translation
// unit that included it -- directly or, far more often, via
// zeitlos.h -- its own private copy. Since almost nothing outside the
// kernel's syscall handlers actually uses them, that produced a
// -Wunused-variable warning pair in essentially every app object
// compiled in this tree: dozens of identical warnings that had to be
// scrolled past to find real ones, which is precisely how a genuine
// silent-truncation bug in repl.c's help text got missed once.
//
// Safe to make extern (checked before changing): nothing anywhere
// mutates either object, and nothing compares a returned pointer
// against &z_ok/&z_fail -- callers on both sides of the syscall
// boundary read the VALUE (`rv->val.uint32 == Z_OK`), never the
// address, so collapsing the per-TU copies into one changes no
// behavior. It also doesn't break the four apps that include this
// header but don't link zobj.o (blinky, bounce, bounceblit, hello):
// an extern declaration only becomes an undefined symbol if something
// actually references it, and none of them do.
extern z_obj_t z_ok;
extern z_obj_t z_fail;

// Function declarations
z_obj_t z_obj_none(void);
z_obj_t z_obj_uint32(uint32_t u);
z_obj_t z_obj_int32(int32_t i);
z_obj_t z_obj_float32(float f);
z_obj_t z_obj_str(const char *s);
z_obj_t z_obj_list(uint32_t len);
z_obj_t z_obj_map(uint32_t len);
z_obj_t z_obj_blob(const void *data, uint32_t len);   // copies data

// blob accessors -- return 0/NULL if obj isn't a Z_BLOB
uint32_t z_blob_len(const z_obj_t *obj);
void *z_blob_data(const z_obj_t *obj);

// Convenience aliases
#define z_obj_int z_obj_int32
#define z_obj_uint z_obj_uint32
#define z_obj_float z_obj_float32

// Table helpers
z_obj_t *z_list_get(z_obj_t *obj, uint32_t index);
z_obj_t *z_map_get_key(z_obj_t *obj, uint32_t index);
z_obj_t *z_map_get_val(z_obj_t *obj, uint32_t index);

// Object management
void z_obj_free(z_obj_t *obj);
void z_obj_print(const z_obj_t *obj);

// Additional helper functions
z_obj_t z_obj_copy(const z_obj_t *src);
int z_obj_equal(const z_obj_t *a, const z_obj_t *b);
uint32_t z_obj_size(const z_obj_t *obj);
z_obj_t *z_map_find(z_obj_t *map, const char *key);
int z_list_append(z_obj_t *list, z_obj_t item);
int z_map_set(z_obj_t *map, const char *key, z_obj_t value);

#endif
