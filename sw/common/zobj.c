/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Object interface.
 *
 */

#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "zobj.h"

// OBJECT CREATION

z_obj_t z_obj_none(void) {
    return (z_obj_t){ .type = Z_NONE, .val.ptr = NULL };
}

z_obj_t z_obj_uint32(uint32_t u) {
    return (z_obj_t){ .type = Z_UINT32, .val.uint32 = u };
}

z_obj_t z_obj_int32(int32_t i) {
    return (z_obj_t){ .type = Z_INT32, .val.int32 = i };
}

z_obj_t z_obj_float32(float f) {
    return (z_obj_t){ .type = Z_FLOAT32, .val.float32 = f };
}

z_obj_t z_obj_str(const char *s) {
    z_obj_t obj = { .type = Z_STR };
    if (s) {
        size_t len = strlen(s);
        obj.val.str = malloc(len + 1);
        if (obj.val.str) {
            strcpy(obj.val.str, s);
        }
    } else {
        obj.val.str = malloc(1);
        if (obj.val.str) {
            obj.val.str[0] = '\0';
        }
    }
    return obj;
}

z_obj_t z_obj_list(uint32_t len) {
    z_obj_table_t *t = malloc(sizeof(z_obj_table_t));
    t->len = len;
    t->a = calloc(len, sizeof(z_obj_t));
    t->b = NULL;

    z_obj_t obj = { .type = Z_LIST };
    obj.val.ptr = t;
    return obj;
}

z_obj_t z_obj_map(uint32_t len) {
    z_obj_table_t *t = malloc(sizeof(z_obj_table_t));
    t->len = len;
    t->a = calloc(len, sizeof(z_obj_t)); // keys
    t->b = calloc(len, sizeof(z_obj_t)); // values
    z_obj_t obj = { .type = Z_MAP };
    obj.val.ptr = t;
    return obj;
}

// RETURN VALUE TESTING

// Check if a return value indicates success
bool z_is_ok(z_obj_t *retobj) {
    return (retobj->type == Z_RETVAL && retobj->val.int32 == 0);
}

// Check if a return value indicates failure
bool z_is_fail(z_obj_t *retobj) {
    return (retobj->type == Z_RETVAL && retobj->val.int32 != 0);
}

// TABLE HELPERS

z_obj_t *z_list_get(z_obj_t *obj, uint32_t index) {
    if (!obj || obj->type != Z_LIST) return NULL;
    z_obj_table_t *t = (z_obj_table_t *)obj->val.ptr;
    if (!t || index >= t->len) return NULL;
    return &t->a[index];
}

z_obj_t *z_map_get_key(z_obj_t *obj, uint32_t index) {
    if (!obj || obj->type != Z_MAP) return NULL;
    z_obj_table_t *t = (z_obj_table_t *)obj->val.ptr;
    if (!t || index >= t->len) return NULL;
    return &t->a[index];
}

z_obj_t *z_map_get_val(z_obj_t *obj, uint32_t index) {
    if (!obj || obj->type != Z_MAP) return NULL;
    z_obj_table_t *t = (z_obj_table_t *)obj->val.ptr;
    if (!t || index >= t->len) return NULL;
    return &t->b[index];
}

// OBJECT FREEING

void z_obj_free(z_obj_t *obj) {
    if (!obj) return;

    switch (obj->type) {
        case Z_STR:
            if (obj->val.str) {
                free(obj->val.str);
                obj->val.str = NULL;
            }
            break;

        case Z_LIST:
        case Z_MAP: {
            z_obj_table_t *t = (z_obj_table_t *)obj->val.ptr;
            if (t) {
                // Free all items in the table
                for (uint32_t i = 0; i < t->len; i++) {
                    z_obj_free(&t->a[i]);           // list items or map keys
                    if (obj->type == Z_MAP && t->b) // map values
                        z_obj_free(&t->b[i]);
                }

                // Free the arrays
                if (t->a) free(t->a);
                if (t->b) free(t->b);
                
                // Free the table structure
                free(t);
                obj->val.ptr = NULL;
            }
            break;
        }

        default:
            // For primitive types (Z_NONE, Z_UINT32, Z_INT32, Z_FLOAT32, Z_RETVAL)
            // no special cleanup needed
            break;
    }

    // Reset to none state
    obj->type = Z_NONE;
    obj->val.ptr = NULL;
}

// OBJECT DEBUG

void z_obj_print(const z_obj_t *obj) {
    if (!obj) {
        printf("NULL");
        return;
    }

    switch (obj->type) {
        case Z_NONE:    
            printf("none"); 
            break;
            
        case Z_UINT32:  
            printf("%u", obj->val.uint32); 
            break;
            
        case Z_INT32:   
            printf("%d", obj->val.int32); 
            break;
            
        case Z_FLOAT32:   
            printf("%.6g", obj->val.float32); 
            break;
            
        case Z_STR:     
            if (obj->val.str) {
                printf("\"%s\"", obj->val.str);
            } else {
                printf("\"NULL\"");
            }
            break;

        case Z_LIST: {
            z_obj_table_t *t = (z_obj_table_t *)obj->val.ptr;
            printf("[");
            if (t) {
                for (uint32_t i = 0; i < t->len; i++) {
                    z_obj_print(&t->a[i]);
                    if (i + 1 < t->len) printf(", ");
                }
            }
            printf("]");
            break;
        }

        case Z_MAP: {
            z_obj_table_t *t = (z_obj_table_t *)obj->val.ptr;
            printf("{");
            if (t) {
                for (uint32_t i = 0; i < t->len; i++) {
                    z_obj_print(&t->a[i]);
                    printf(": ");
                    z_obj_print(&t->b[i]);
                    if (i + 1 < t->len) printf(", ");
                }
            }
            printf("}");
            break;
        }

        case Z_RETVAL:
            printf("retval(%d)", obj->val.int32);
            break;

        default:
            printf("<unknown:%d>", obj->type);
            break;
    }
}

// HELPER FUNCTIONS

// Deep copy an object
z_obj_t z_obj_copy(const z_obj_t *src) {
    if (!src) return z_obj_none();
    
    switch (src->type) {
        case Z_NONE:
            return z_obj_none();
            
        case Z_UINT32:
            return z_obj_uint32(src->val.uint32);
            
        case Z_INT32:
            return z_obj_int32(src->val.int32);
            
        case Z_FLOAT32:
            return z_obj_float32(src->val.float32);
            
        case Z_STR:
            return z_obj_str(src->val.str ? src->val.str : "");
            
        case Z_LIST: {
            z_obj_table_t *src_table = (z_obj_table_t *)src->val.ptr;
            if (!src_table) return z_obj_none();
            
            z_obj_t new_list = z_obj_list(src_table->len);
            z_obj_table_t *new_table = (z_obj_table_t *)new_list.val.ptr;
            
            for (uint32_t i = 0; i < src_table->len; i++) {
                new_table->a[i] = z_obj_copy(&src_table->a[i]);
            }
            
            return new_list;
        }
        
        case Z_MAP: {
            z_obj_table_t *src_table = (z_obj_table_t *)src->val.ptr;
            if (!src_table) return z_obj_none();
            
            z_obj_t new_map = z_obj_map(src_table->len);
            z_obj_table_t *new_table = (z_obj_table_t *)new_map.val.ptr;
            
            for (uint32_t i = 0; i < src_table->len; i++) {
                new_table->a[i] = z_obj_copy(&src_table->a[i]);
                new_table->b[i] = z_obj_copy(&src_table->b[i]);
            }
            
            return new_map;
        }
        
        case Z_RETVAL:
            return (z_obj_t){ .type = Z_RETVAL, .val.int32 = src->val.int32 };
        
        default:
            return z_obj_none();
    }
}

// Compare two objects for equality
int z_obj_equal(const z_obj_t *a, const z_obj_t *b) {
    if (!a || !b) return (a == b);
    
    if (a->type != b->type) return 0;
    
    switch (a->type) {
        case Z_NONE:
            return 1;
            
        case Z_UINT32:
            return a->val.uint32 == b->val.uint32;
            
        case Z_INT32:
            return a->val.int32 == b->val.int32;
            
        case Z_FLOAT32:
            // Use epsilon comparison for floats
            return fabsf(a->val.float32 - b->val.float32) < 1e-6f;
            
        case Z_STR:
            if (!a->val.str || !b->val.str) return (a->val.str == b->val.str);
            return strcmp(a->val.str, b->val.str) == 0;
            
        case Z_LIST: {
            z_obj_table_t *ta = (z_obj_table_t *)a->val.ptr;
            z_obj_table_t *tb = (z_obj_table_t *)b->val.ptr;
            
            if (!ta || !tb) return (ta == tb);
            if (ta->len != tb->len) return 0;
            
            for (uint32_t i = 0; i < ta->len; i++) {
                if (!z_obj_equal(&ta->a[i], &tb->a[i])) return 0;
            }
            return 1;
        }
        
        case Z_MAP: {
            z_obj_table_t *ta = (z_obj_table_t *)a->val.ptr;
            z_obj_table_t *tb = (z_obj_table_t *)b->val.ptr;
            
            if (!ta || !tb) return (ta == tb);
            if (ta->len != tb->len) return 0;
            
            for (uint32_t i = 0; i < ta->len; i++) {
                if (!z_obj_equal(&ta->a[i], &tb->a[i])) return 0;
                if (!z_obj_equal(&ta->b[i], &tb->b[i])) return 0;
            }
            return 1;
        }
        
        case Z_RETVAL:
            return a->val.int32 == b->val.int32;
        
        default:
            return 0;
    }
}

// Get the size of a container object
uint32_t z_obj_size(const z_obj_t *obj) {
    if (!obj) return 0;
    
    switch (obj->type) {
        case Z_LIST:
        case Z_MAP: {
            z_obj_table_t *t = (z_obj_table_t *)obj->val.ptr;
            return t ? t->len : 0;
        }
        
        case Z_STR:
            return obj->val.str ? (uint32_t)strlen(obj->val.str) : 0;
            
        default:
            return 0;
    }
}

// Find a value in a map by string key
z_obj_t *z_map_find(z_obj_t *map, const char *key) {
    if (!map || map->type != Z_MAP || !key) return NULL;
    
    z_obj_table_t *table = (z_obj_table_t *)map->val.ptr;
    if (!table) return NULL;
    
    for (uint32_t i = 0; i < table->len; i++) {
        z_obj_t *map_key = &table->a[i];
        if (map_key->type == Z_STR && 
            map_key->val.str && 
            strcmp(map_key->val.str, key) == 0) {
            return &table->b[i];
        }
    }
    
    return NULL;
}

// Append an item to a list (dynamic growth)
int z_list_append(z_obj_t *list, z_obj_t item) {
    if (!list || list->type != Z_LIST) return 0;
    
    z_obj_table_t *t = (z_obj_table_t *)list->val.ptr;
    if (!t) return 0;
    
    // For simplicity, this implementation requires pre-allocated space
    // In a real implementation, you might want to grow the array dynamically
    // For now, we'll find the first Z_NONE slot
    for (uint32_t i = 0; i < t->len; i++) {
        if (t->a[i].type == Z_NONE) {
            t->a[i] = z_obj_copy(&item);
            return 1;
        }
    }
    
    return 0; // No space available
}

// Set a key-value pair in a map
int z_map_set(z_obj_t *map, const char *key, z_obj_t value) {
    if (!map || map->type != Z_MAP || !key) return 0;
    
    z_obj_table_t *t = (z_obj_table_t *)map->val.ptr;
    if (!t) return 0;
    
    // First, try to find existing key
    for (uint32_t i = 0; i < t->len; i++) {
        z_obj_t *map_key = &t->a[i];
        if (map_key->type == Z_STR && 
            map_key->val.str && 
            strcmp(map_key->val.str, key) == 0) {
            // Key exists, update value
            z_obj_free(&t->b[i]);
            t->b[i] = z_obj_copy(&value);
            return 1;
        }
    }
    
    // Key doesn't exist, find empty slot
    for (uint32_t i = 0; i < t->len; i++) {
        if (t->a[i].type == Z_NONE) {
            t->a[i] = z_obj_str(key);
            t->b[i] = z_obj_copy(&value);
            return 1;
        }
    }
    
    return 0; // No space available
}
