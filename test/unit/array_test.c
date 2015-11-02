#include <check.h>
#include "../../src/array.h"

START_TEST (test_array_usage)
{
    Array a;
    array_init(&a, sizeof(int), 2);
    for (int i = 0; i < 5; ++i)
        array_insert(&a, &i);
    ck_assert_int_eq(array_size(&a), 5);

    for (int i = 0; i < 5; ++i) {
        ck_assert_int_eq(i, *(int*)array_get(&a, i));
    }

    array_free(&a);
}
END_TEST

START_TEST (test_array_pop)
{
    Array a;
    array_init(&a, sizeof(int), 2);
    for (int i = 0; i < 5; ++i)
        array_insert(&a, &i);

    int *x = array_pop(&a);
    int *y = array_pop(&a);
    ck_assert_int_eq(0, *x);
    ck_assert_int_eq(1, *y);

    ck_assert_int_eq(array_size(&a), 3);
    ck_assert_int_eq(*(int*)array_get(&a, 0), 2);

    array_free(&a);
}
END_TEST

START_TEST (test_array2d_usage) {
    Array2D a;
    array2d_init(&a, sizeof(int), 1, 2);
    for (int i = 0; i < 5; ++i) {
        if (i) array2d_linefeed(&a);
        for (int j = 0; j < 5; ++j) {
            int z = i * j;
            array2d_insert(&a, &z);
        }
    }
    ck_assert_int_eq(array2d_size(&a), 5);

    for (int i = 0; i < 5; ++i) {
        Array *row = array2d_row(&a, i);
        ck_assert_int_eq(array_size(row), 5);
        for (int j = 0; j < 5; ++j) {
            ck_assert_int_eq(i * j, *(int*)array2d_get(&a, i, j));
            ck_assert_int_eq(i * j, *(int*)array_get(row, j));
        }
    }

    array2d_free(&a);
}
END_TEST


START_TEST (test_array2d_pop) {
    Array2D a;
    array2d_init(&a, sizeof(int), 1, 2);
    for (int i = 0; i < 5; ++i) {
        if (i) array2d_linefeed(&a);
        for (int j = 0; j < 5; ++j) {
            int z = i * j;
            array2d_insert(&a, &z);
        }
    }

    Array *top = array2d_pop_row(&a);
    ck_assert_int_eq(array2d_size(&a), 4);
    ck_assert_int_eq(array_size(top), 5);
    for (int j = 0; j < 5; ++j)
        ck_assert_int_eq(*(int*)array_get(top, j), 0);

    array_free(top);
    array2d_free(&a);
}
END_TEST

TCase *ArrayTCase(void)
{
    TCase *tc;

    /* Core test case */
    tc = tcase_create("Array");

    tcase_add_test(tc, test_array_usage);
    tcase_add_test(tc, test_array2d_usage);
    tcase_add_test(tc, test_array_pop);
    tcase_add_test(tc, test_array2d_pop);

    return tc;
}
