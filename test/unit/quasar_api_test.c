#include <stdlib.h>
#include <check.h>
#include <string.h>
#include "tests.h"
#include "../../src/quasar_api.h"
#include "postgres.h"
#include "nodes/pg_list.h"

#define QUASAR_URL "http://localhost:8080"
#define QUASAR_PATH "/local/quasar"
#define QUASAR_API_GET(query, resp) \
    quasar_api_get(QUASAR_URL, QUASAR_PATH, (query), (resp))
#define CK_LIST_LEN(l, exp) \
    ck_assert_int_eq(list_length((l)), (exp))
#define CK_LIST_NTH(l, i, exp) \
    ck_assert_str_eq(list_nth((l), (i)), (exp));

#define CK_LIST2D_ROWS(l, exp) \
    CK_LIST_LEN((l), (exp))
#define CK_LIST2D_COLS(l, lc, exp)              \
    foreach(lc, (l)) {                          \
        CK_LIST_LEN(lfirst((lc)), (exp));       \
    }
#define CK_LIST2D_SIZE(l, lc, exp_rows, exp_cols) \
    CK_LIST2D_ROWS((l), (exp_rows));              \
    CK_LIST2D_COLS((l), lc, (exp_cols))
#define CK_LIST2D_NTH(l, i, j, exp) \
    CK_LIST_NTH(list_nth((l), (i)), (j), (exp))


START_TEST(test_basic_api_get)
{
    QuasarApiResponse resp;
    int res;
    ListCell *lc;

    res = QUASAR_API_GET("select city from zips limit 3", &resp);

    // Ensure we got a succesful response
    ck_assert_int_eq(res, QUASAR_API_SUCCESS);

    // Check columnNames array
    CK_LIST_LEN(resp.columnNames, 1);
    CK_LIST_NTH(resp.columnNames, 0, "city");

    // Check data array
    CK_LIST2D_SIZE(resp.data, lc, 3, 1);
    CK_LIST2D_NTH(resp.data, 0, 0, "BARRE");
    CK_LIST2D_NTH(resp.data, 1, 0, "AGAWAM");
    CK_LIST2D_NTH(resp.data, 2, 0, "BLANDFORD");

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
