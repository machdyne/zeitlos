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
    Z_MAP
} z_type_t;

typedef struct {
    z_type_t type;
    union {
        void *ptr;          // used for lists, maps
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

// Return value objects
static z_obj_t z_ok = { .type = Z_RETVAL, .val.int32 = 0 };
static z_obj_t z_fail = { .type = Z_RETVAL, .val.int32 = 1 };

// Function declarations
z_obj_t z_obj_none(void);
z_obj_t z_obj_uint32(uint32_t u);
z_obj_t z_obj_int32(int32_t i);
z_obj_t z_obj_float32(float f);
z_obj_t z_obj_str(const char *s);
z_obj_t z_obj_list(uint32_t len);
z_obj_t z_obj_map(uint32_t len);

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
