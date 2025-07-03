/*
 * Test suite for zobj object system
 * Copyright (c) 2025 Test Suite
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "zobj.h"

// Test counters
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

// Test macros
#define TEST_START(name) \
    do { \
        printf("Running test: %s\n", name); \
        tests_run++; \
    } while(0)

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("  ✓ %s\n", message); \
        } else { \
            printf("  ✗ %s\n", message); \
            tests_failed++; \
            return 0; \
        } \
    } while(0)

#define TEST_END() \
    do { \
        tests_passed++; \
        printf("  Test passed\n\n"); \
        return 1; \
    } while(0)

// Test functions
int test_none_object(void) {
    TEST_START("none object creation");
    
    z_obj_t obj = z_obj_none();
    TEST_ASSERT(obj.type == Z_NONE, "none object has correct type");
    TEST_ASSERT(obj.val.ptr == NULL, "none object has null pointer");
    
    TEST_END();
}

int test_uint32_object(void) {
    TEST_START("uint32 object creation");
    
    z_obj_t obj = z_obj_uint32(42U);
    TEST_ASSERT(obj.type == Z_UINT32, "uint32 object has correct type");
    TEST_ASSERT(obj.val.uint32 == 42U, "uint32 object has correct value");
    
    z_obj_t large_obj = z_obj_uint32(4294967295U);
    TEST_ASSERT(large_obj.type == Z_UINT32, "large uint32 object has correct type");
    TEST_ASSERT(large_obj.val.uint32 == 4294967295U, "large uint32 object has correct value");
    
    TEST_END();
}

int test_int32_object(void) {
    TEST_START("int32 object creation");
    
    z_obj_t obj = z_obj_int32(42);
    TEST_ASSERT(obj.type == Z_INT32, "int32 object has correct type");
    TEST_ASSERT(obj.val.int32 == 42, "int32 object has correct value");
    
    z_obj_t neg_obj = z_obj_int32(-123);
    TEST_ASSERT(neg_obj.type == Z_INT32, "negative int32 object has correct type");
    TEST_ASSERT(neg_obj.val.int32 == -123, "negative int32 object has correct value");
    
    TEST_END();
}

int test_float32_object(void) {
    TEST_START("float32 object creation");
    
    z_obj_t obj = z_obj_float32(3.14159f);
    TEST_ASSERT(obj.type == Z_FLOAT32, "float32 object has correct type");
    TEST_ASSERT(fabsf(obj.val.float32 - 3.14159f) < 1e-6f, "float32 object has correct value");
    
    z_obj_t neg_obj = z_obj_float32(-2.718f);
    TEST_ASSERT(neg_obj.type == Z_FLOAT32, "negative float32 object has correct type");
    TEST_ASSERT(fabsf(neg_obj.val.float32 - (-2.718f)) < 1e-6f, "negative float32 object has correct value");
    
    z_obj_t zero_obj = z_obj_float32(0.0f);
    TEST_ASSERT(zero_obj.type == Z_FLOAT32, "zero float32 object has correct type");
    TEST_ASSERT(zero_obj.val.float32 == 0.0f, "zero float32 object has correct value");
    
    TEST_END();
}

int test_string_object(void) {
    TEST_START("string object creation");
    
    const char *test_str = "Hello, World!";
    z_obj_t obj = z_obj_str(test_str);
    
    TEST_ASSERT(obj.type == Z_STR, "string object has correct type");
    TEST_ASSERT(obj.val.str != NULL, "string object has non-null string");
    TEST_ASSERT(strcmp(obj.val.str, test_str) == 0, "string object has correct value");
    TEST_ASSERT(obj.val.str != test_str, "string object uses copied string");
    
    z_obj_free(&obj);
    TEST_ASSERT(obj.type == Z_NONE, "freed string object has none type");
    TEST_ASSERT(obj.val.ptr == NULL, "freed string object has null pointer");
    
    TEST_END();
}

int test_empty_string_object(void) {
    TEST_START("empty string object creation");
    
    z_obj_t obj = z_obj_str("");
    TEST_ASSERT(obj.type == Z_STR, "empty string object has correct type");
    TEST_ASSERT(obj.val.str != NULL, "empty string object has non-null string");
    TEST_ASSERT(strlen(obj.val.str) == 0, "empty string object has zero length");
    
    z_obj_free(&obj);
    TEST_END();
}

int test_list_object(void) {
    TEST_START("list object creation");
    
    z_obj_t list = z_obj_list(3);
    TEST_ASSERT(list.type == Z_LIST, "list object has correct type");
    TEST_ASSERT(list.val.ptr != NULL, "list object has non-null pointer");
    
    z_obj_table_t *table = (z_obj_table_t *)list.val.ptr;
    TEST_ASSERT(table->len == 3, "list object has correct length");
    TEST_ASSERT(table->a != NULL, "list object has allocated array");
    TEST_ASSERT(table->b == NULL, "list object has null b array");
    
    z_obj_free(&list);
    TEST_ASSERT(list.type == Z_NONE, "freed list object has none type");
    
    TEST_END();
}

int test_map_object(void) {
    TEST_START("map object creation");
    
    z_obj_t map = z_obj_map(5);
    TEST_ASSERT(map.type == Z_MAP, "map object has correct type");
    TEST_ASSERT(map.val.ptr != NULL, "map object has non-null pointer");
    
    z_obj_table_t *table = (z_obj_table_t *)map.val.ptr;
    TEST_ASSERT(table->len == 5, "map object has correct length");
    TEST_ASSERT(table->a != NULL, "map object has allocated key array");
    TEST_ASSERT(table->b != NULL, "map object has allocated value array");
    
    z_obj_free(&map);
    TEST_ASSERT(map.type == Z_NONE, "freed map object has none type");
    
    TEST_END();
}

int test_list_access(void) {
    TEST_START("list access functions");
    
    z_obj_t list = z_obj_list(3);
    
    // Test valid access
    z_obj_t *item = z_list_get(&list, 0);
    TEST_ASSERT(item != NULL, "list item 0 is accessible");
    TEST_ASSERT(item->type == Z_NONE, "list item 0 is initially none");
    
    item = z_list_get(&list, 2);
    TEST_ASSERT(item != NULL, "list item 2 is accessible");
    
    // Test invalid access
    item = z_list_get(&list, 3);
    TEST_ASSERT(item == NULL, "list item 3 is not accessible (out of bounds)");
    
    item = z_list_get(&list, 100);
    TEST_ASSERT(item == NULL, "list item 100 is not accessible (out of bounds)");
    
    // Test with wrong type
    z_obj_t not_list = z_obj_int32(42);
    item = z_list_get(&not_list, 0);
    TEST_ASSERT(item == NULL, "list access fails on non-list object");
    
    z_obj_free(&list);
    TEST_END();
}

int test_map_access(void) {
    TEST_START("map access functions");
    
    z_obj_t map = z_obj_map(2);
    
    // Test key access
    z_obj_t *key = z_map_get_key(&map, 0);
    TEST_ASSERT(key != NULL, "map key 0 is accessible");
    TEST_ASSERT(key->type == Z_NONE, "map key 0 is initially none");
    
    key = z_map_get_key(&map, 1);
    TEST_ASSERT(key != NULL, "map key 1 is accessible");
    
    // Test value access
    z_obj_t *val = z_map_get_val(&map, 0);
    TEST_ASSERT(val != NULL, "map value 0 is accessible");
    TEST_ASSERT(val->type == Z_NONE, "map value 0 is initially none");
    
    val = z_map_get_val(&map, 1);
    TEST_ASSERT(val != NULL, "map value 1 is accessible");
    
    // Test out of bounds
    key = z_map_get_key(&map, 2);
    TEST_ASSERT(key == NULL, "map key 2 is not accessible (out of bounds)");
    
    val = z_map_get_val(&map, 2);
    TEST_ASSERT(val == NULL, "map value 2 is not accessible (out of bounds)");
    
    // Test with wrong type
    z_obj_t not_map = z_obj_int32(42);
    key = z_map_get_key(&not_map, 0);
    TEST_ASSERT(key == NULL, "map key access fails on non-map object");
    
    val = z_map_get_val(&not_map, 0);
    TEST_ASSERT(val == NULL, "map value access fails on non-map object");
    
    z_obj_free(&map);
    TEST_END();
}

int test_nested_structures(void) {
    TEST_START("nested data structures");
    
    z_obj_t list = z_obj_list(2);
    
    // Add a string to the list
    z_obj_t *item0 = z_list_get(&list, 0);
    *item0 = z_obj_str("Hello");
    
    // Add a nested list
    z_obj_t *item1 = z_list_get(&list, 1);
    *item1 = z_obj_list(1);
    
    // Add an int to the nested list
    z_obj_t *nested_item = z_list_get(item1, 0);
    *nested_item = z_obj_int32(42);
    
    // Verify structure
    TEST_ASSERT(item0->type == Z_STR, "first item is string");
    TEST_ASSERT(strcmp(item0->val.str, "Hello") == 0, "first item has correct value");
    
    TEST_ASSERT(item1->type == Z_LIST, "second item is list");
    TEST_ASSERT(nested_item->type == Z_INT32, "nested item is int");
    TEST_ASSERT(nested_item->val.int32 == 42, "nested item has correct value");
    
    z_obj_free(&list);
    TEST_END();
}

int test_complex_map(void) {
    TEST_START("complex map operations");
    
    z_obj_t map = z_obj_map(4);
    
    // Set up key-value pairs with different types
    z_obj_t *key0 = z_map_get_key(&map, 0);
    z_obj_t *val0 = z_map_get_val(&map, 0);
    *key0 = z_obj_str("name");
    *val0 = z_obj_str("Alice");
    
    z_obj_t *key1 = z_map_get_key(&map, 1);
    z_obj_t *val1 = z_map_get_val(&map, 1);
    *key1 = z_obj_str("age");
    *val1 = z_obj_int32(30);
    
    z_obj_t *key2 = z_map_get_key(&map, 2);
    z_obj_t *val2 = z_map_get_val(&map, 2);
    *key2 = z_obj_str("score");
    *val2 = z_obj_float32(95.5f);
    
    z_obj_t *key3 = z_map_get_key(&map, 3);
    z_obj_t *val3 = z_map_get_val(&map, 3);
    *key3 = z_obj_str("hobbies");
    *val3 = z_obj_list(2);
    
    // Add items to the hobbies list
    z_obj_t *hobby0 = z_list_get(val3, 0);
    z_obj_t *hobby1 = z_list_get(val3, 1);
    *hobby0 = z_obj_str("reading");
    *hobby1 = z_obj_str("coding");
    
    // Verify structure
    TEST_ASSERT(key0->type == Z_STR && strcmp(key0->val.str, "name") == 0, 
                "key0 is correct");
    TEST_ASSERT(val0->type == Z_STR && strcmp(val0->val.str, "Alice") == 0, 
                "val0 is correct");
    
    TEST_ASSERT(key1->type == Z_STR && strcmp(key1->val.str, "age") == 0, 
                "key1 is correct");
    TEST_ASSERT(val1->type == Z_INT32 && val1->val.int32 == 30, 
                "val1 is correct");
    
    TEST_ASSERT(key2->type == Z_STR && strcmp(key2->val.str, "score") == 0, 
                "key2 is correct");
    TEST_ASSERT(val2->type == Z_FLOAT32 && fabsf(val2->val.float32 - 95.5f) < 1e-6f, 
                "val2 is correct");
    
    TEST_ASSERT(key3->type == Z_STR && strcmp(key3->val.str, "hobbies") == 0, 
                "key3 is correct");
    TEST_ASSERT(val3->type == Z_LIST, "val3 is a list");
    
    TEST_ASSERT(hobby0->type == Z_STR && strcmp(hobby0->val.str, "reading") == 0, 
                "hobby0 is correct");
    TEST_ASSERT(hobby1->type == Z_STR && strcmp(hobby1->val.str, "coding") == 0, 
                "hobby1 is correct");
    
    z_obj_free(&map);
    TEST_END();
}

int test_zero_length_containers(void) {
    TEST_START("zero length containers");
    
    z_obj_t list = z_obj_list(0);
    TEST_ASSERT(list.type == Z_LIST, "zero-length list has correct type");
    
    z_obj_t *item = z_list_get(&list, 0);
    TEST_ASSERT(item == NULL, "zero-length list returns null for any index");
    
    z_obj_t map = z_obj_map(0);
    TEST_ASSERT(map.type == Z_MAP, "zero-length map has correct type");
    
    z_obj_t *key = z_map_get_key(&map, 0);
    z_obj_t *val = z_map_get_val(&map, 0);
    TEST_ASSERT(key == NULL, "zero-length map returns null key for any index");
    TEST_ASSERT(val == NULL, "zero-length map returns null value for any index");
    
    z_obj_free(&list);
    z_obj_free(&map);
    TEST_END();
}

int test_memory_safety(void) {
    TEST_START("memory safety");
    
    // Test freeing none object
    z_obj_t none_obj = z_obj_none();
    z_obj_free(&none_obj);  // Should not crash
    
    // Test freeing already freed object
    z_obj_t str_obj = z_obj_str("test");
    z_obj_free(&str_obj);
    z_obj_free(&str_obj);  // Should not crash (already none)
    
    // Test freeing with null pointer
    z_obj_free(NULL);  // Should not crash
    
    TEST_ASSERT(1, "memory safety tests completed without crash");
    TEST_END();
}

int test_mixed_types_in_containers(void) {
    TEST_START("mixed types in containers");
    
    z_obj_t list = z_obj_list(5);
    
    // Fill list with different types
    z_obj_t *items[5];
    for (int i = 0; i < 5; i++) {
        items[i] = z_list_get(&list, i);
    }
    
    *items[0] = z_obj_none();
    *items[1] = z_obj_uint32(42U);
    *items[2] = z_obj_int32(-42);
    *items[3] = z_obj_float32(3.14f);
    *items[4] = z_obj_str("test");
    
    // Verify types
    TEST_ASSERT(items[0]->type == Z_NONE, "item 0 is none");
    TEST_ASSERT(items[1]->type == Z_UINT32 && items[1]->val.uint32 == 42U, "item 1 is uint32");
    TEST_ASSERT(items[2]->type == Z_INT32 && items[2]->val.int32 == -42, "item 2 is int32");
    TEST_ASSERT(items[3]->type == Z_FLOAT32 && fabsf(items[3]->val.float32 - 3.14f) < 1e-6f, "item 3 is float32");
    TEST_ASSERT(items[4]->type == Z_STR && strcmp(items[4]->val.str, "test") == 0, "item 4 is string");
    
    z_obj_free(&list);
    TEST_END();
}

int test_retval_objects(void) {
    TEST_START("return value objects");
    
    // Test the static return value objects
    TEST_ASSERT(z_ok.type == Z_RETVAL, "z_ok has correct type");
    TEST_ASSERT(z_ok.val.int32 == 0, "z_ok has correct value");
    
    TEST_ASSERT(z_fail.type == Z_RETVAL, "z_fail has correct type");
    TEST_ASSERT(z_fail.val.int32 == 1, "z_fail has correct value");
    
    // Test creating custom return values
    z_obj_t custom_retval = { .type = Z_RETVAL, .val.uint32 = 42 };
    TEST_ASSERT(custom_retval.type == Z_RETVAL, "custom retval has correct type");
    TEST_ASSERT(custom_retval.val.uint32 == 42, "custom retval has correct value");
    
    TEST_END();
}

int test_edge_cases(void) {
    TEST_START("edge cases");
    
    // Test very large numbers
    z_obj_t max_uint = z_obj_uint32(UINT32_MAX);
    TEST_ASSERT(max_uint.val.uint32 == UINT32_MAX, "maximum uint32 value");
    
    z_obj_t min_int = z_obj_int32(INT32_MIN);
    TEST_ASSERT(min_int.val.int32 == INT32_MIN, "minimum int32 value");
    
    z_obj_t max_int = z_obj_int32(INT32_MAX);
    TEST_ASSERT(max_int.val.int32 == INT32_MAX, "maximum int32 value");
    
    // Test special float values
    z_obj_t zero_float = z_obj_float32(0.0f);
    TEST_ASSERT(zero_float.val.float32 == 0.0f, "zero float");
    
    z_obj_t neg_zero_float = z_obj_float32(-0.0f);
    TEST_ASSERT(neg_zero_float.val.float32 == -0.0f, "negative zero float");
    
    // Note: NaN and infinity tests would require special handling
    
    TEST_END();
}

int test_copy_function(void) {
    TEST_START("object copy function");
    
    // Test copying simple types
    z_obj_t orig_int = z_obj_int32(42);
    z_obj_t copy_int = z_obj_copy(&orig_int);
    TEST_ASSERT(z_obj_equal(&orig_int, &copy_int), "int32 copy is equal to original");
    TEST_ASSERT(copy_int.type == Z_INT32 && copy_int.val.int32 == 42, "int32 copy has correct value");
    
    z_obj_t orig_str = z_obj_str("hello");
    z_obj_t copy_str = z_obj_copy(&orig_str);
    TEST_ASSERT(z_obj_equal(&orig_str, &copy_str), "string copy is equal to original");
    TEST_ASSERT(copy_str.val.str != orig_str.val.str, "string copy has different pointer");
    
    // Test copying complex types
    z_obj_t orig_list = z_obj_list(2);
    z_obj_t *item0 = z_list_get(&orig_list, 0);
    z_obj_t *item1 = z_list_get(&orig_list, 1);
    *item0 = z_obj_int32(1);
    *item1 = z_obj_str("two");
    
    z_obj_t copy_list = z_obj_copy(&orig_list);
    TEST_ASSERT(z_obj_equal(&orig_list, &copy_list), "list copy is equal to original");
    TEST_ASSERT(copy_list.val.ptr != orig_list.val.ptr, "list copy has different pointer");
    
    z_obj_free(&orig_str);
    z_obj_free(&copy_str);
    z_obj_free(&orig_list);
    z_obj_free(&copy_list);
    
    TEST_END();
}

int test_map_find_function(void) {
    TEST_START("map find function");
    
    z_obj_t map = z_obj_map(3);
    
    // Set up map
    z_obj_t *key0 = z_map_get_key(&map, 0);
    z_obj_t *val0 = z_map_get_val(&map, 0);
    *key0 = z_obj_str("name");
    *val0 = z_obj_str("Alice");
    
    z_obj_t *key1 = z_map_get_key(&map, 1);
    z_obj_t *val1 = z_map_get_val(&map, 1);
    *key1 = z_obj_str("age");
    *val1 = z_obj_int32(25);
    
    z_obj_t *key2 = z_map_get_key(&map, 2);
    z_obj_t *val2 = z_map_get_val(&map, 2);
    *key2 = z_obj_int32(123);  // Non-string key
    *val2 = z_obj_str("numeric_key");
    
    // Test finding existing keys
    z_obj_t *found = z_map_find(&map, "name");
    TEST_ASSERT(found != NULL, "found existing string key 'name'");
    TEST_ASSERT(found->type == Z_STR && strcmp(found->val.str, "Alice") == 0, "found correct value for 'name'");
    
    found = z_map_find(&map, "age");
    TEST_ASSERT(found != NULL, "found existing string key 'age'");
    TEST_ASSERT(found->type == Z_INT32 && found->val.int32 == 25, "found correct value for 'age'");
    
    // Test finding non-existent key
    found = z_map_find(&map, "nonexistent");
    TEST_ASSERT(found == NULL, "did not find non-existent key");
    
    // Test with wrong object type
    z_obj_t not_map = z_obj_int32(42);
    found = z_map_find(&not_map, "key");
    TEST_ASSERT(found == NULL, "map find fails on non-map object");
    
    z_obj_free(&map);
    TEST_END();
}

int test_convenience_aliases(void) {
    TEST_START("convenience aliases");
    
    // Test that aliases work correctly
    z_obj_t int_obj = z_obj_int(42);
    TEST_ASSERT(int_obj.type == Z_INT32, "z_obj_int alias works");
    TEST_ASSERT(int_obj.val.int32 == 42, "z_obj_int alias has correct value");
    
    z_obj_t uint_obj = z_obj_uint(42U);
    TEST_ASSERT(uint_obj.type == Z_UINT32, "z_obj_uint alias works");
    TEST_ASSERT(uint_obj.val.uint32 == 42U, "z_obj_uint alias has correct value");
    
    z_obj_t float_obj = z_obj_float(3.14f);
    TEST_ASSERT(float_obj.type == Z_FLOAT32, "z_obj_float alias works");
    TEST_ASSERT(fabsf(float_obj.val.float32 - 3.14f) < 1e-6f, "z_obj_float alias has correct value");
    
    TEST_END();
}

// Print test summary
void print_test_summary(void) {
    printf("=== Test Summary ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("All tests passed! ✓\n");
    } else {
        printf("Some tests failed! ✗\n");
    }
}

// Manual test with print output
void manual_test_print(void) {
    printf("=== Manual Print Test ===\n");
    
    // Test various object types
    printf("None object: ");
    z_obj_t none_obj = z_obj_none();
    z_obj_print(&none_obj);
    printf("\n");
    
    printf("Uint32 object: ");
    z_obj_t uint32_obj = z_obj_uint32(42U);
    z_obj_print(&uint32_obj);
    printf("\n");
    
    printf("Int32 object: ");
    z_obj_t int32_obj = z_obj_int32(-42);
    z_obj_print(&int32_obj);
    printf("\n");
    
    printf("Float32 object: ");
    z_obj_t float32_obj = z_obj_float32(3.14159f);
    z_obj_print(&float32_obj);
    printf("\n");
    
    printf("String object: ");
    z_obj_t str_obj = z_obj_str("Hello");
    z_obj_print(&str_obj);
    printf("\n");
    
    printf("List object: ");
    z_obj_t list_obj = z_obj_list(4);
    z_obj_t *item0 = z_list_get(&list_obj, 0);
    z_obj_t *item1 = z_list_get(&list_obj, 1);
    z_obj_t *item2 = z_list_get(&list_obj, 2);
    z_obj_t *item3 = z_list_get(&list_obj, 3);
    *item0 = z_obj_uint32(1U);
    *item1 = z_obj_str("two");
    *item2 = z_obj_float32(3.0f);
    *item3 = z_obj_int32(-4);
    z_obj_print(&list_obj);
    printf("\n");
    
    printf("Map object: ");
    z_obj_t map_obj = z_obj_map(3);
    z_obj_t *key0 = z_map_get_key(&map_obj, 0);
    z_obj_t *val0 = z_map_get_val(&map_obj, 0);
    z_obj_t *key1 = z_map_get_key(&map_obj, 1);
    z_obj_t *val1 = z_map_get_val(&map_obj, 1);
    z_obj_t *key2 = z_map_get_key(&map_obj, 2);
    z_obj_t *val2 = z_map_get_val(&map_obj, 2);
    *key0 = z_obj_str("name");
    *val0 = z_obj_str("Alice");
    *key1 = z_obj_str("age");
    *val1 = z_obj_int32(25);
    *key2 = z_obj_str("score");
    *val2 = z_obj_float32(98.5f);
    z_obj_print(&map_obj);
    printf("\n");
    
    printf("Return value objects: ");
    z_obj_print(&z_ok);
    printf(" ");
    z_obj_print(&z_fail);
    printf("\n");
    
    // Clean up
    z_obj_free(&str_obj);
    z_obj_free(&list_obj);
    z_obj_free(&map_obj);
    
    printf("\n");
}

int main(void) {
    printf("=== ZObj Test Suite ===\n\n");
    
    // Run all tests
    test_none_object();
    test_uint32_object();
    test_int32_object();
    test_float32_object();
    test_string_object();
    test_empty_string_object();
    test_list_object();
    test_map_object();
    test_list_access();
    test_map_access();
    test_nested_structures();
    test_complex_map();
    test_zero_length_containers();
    test_memory_safety();
    test_mixed_types_in_containers();
    test_retval_objects();
    test_edge_cases();
    test_copy_function();
    test_map_find_function();
    test_convenience_aliases();
    
    print_test_summary();
    printf("\n");
    
    manual_test_print();
    
    return (tests_failed == 0) ? 0 : 1;
}
