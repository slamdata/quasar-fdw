#include <stdlib.h>
#include <check.h>
#include "tests.h"
#include "../../src/quasar_api.h"
#include "../../src/array.h"

#define QUASAR_URL "http://localhost:8080"
#define QUASAR_PATH "/local/quasar"
#define QUASAR_API_GET(query, resp) quasar_api_get(QUASAR_URL, QUASAR_PATH, query, resp)


START_TEST(test_basic_api_get)
{
    QuasarApiResponse resp;
    int res;

    res = QUASAR_API_GET("select city from zips limit 3", &resp);
    ck_assert_int_eq(res, QUASAR_API_SUCCESS);
    ck_assert_int_eq(array_size(resp.columnNames), 1);
    ck_assert_str_eq(array_get_str(resp.columnNames, 0), "city");
    ck_assert_int_eq(array2d_size(resp.data), 3);
    ck_assert_str_eq(array2d_get_str(resp.data, 0, 0), "BARRE");
    ck_assert_int_eq(array_size(array2d_row(resp.data, 0)), 1);
    ck_assert_str_eq(array2d_get_str(resp.data, 1, 0), "AGAWAM");
    ck_assert_int_eq(array_size(array2d_row(resp.data, 1)), 1);
    ck_assert_str_eq(array2d_get_str(resp.data, 2, 0), "BLANDFORD");
    ck_assert_int_eq(array_size(array2d_row(resp.data, 2)), 1);
    quasar_api_response_free(&resp);
}
END_TEST

START_TEST (test_setup)
{
    quasar_api_init();
}
END_TEST
START_TEST (test_teardown)
{
    quasar_api_cleanup();
}
END_TEST

TCase * QuasarApiTCase(void) {
    TCase *tc;

    /* Core test case */
    tc = tcase_create("Quasar Api");

    tcase_add_test(tc, test_setup);
    tcase_add_test(tc, test_basic_api_get);
    tcase_add_test(tc, test_teardown);

    return tc;
}
