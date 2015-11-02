/*-------------------------------------------------------------------------
 *
 * Quasar Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2015 SlamData
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Jon Eisen <jon@joneisen.works>
 *
 * IDENTIFICATION
 *		  quasar_fdw/test/unit/unit-test.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdlib.h>
#include <check.h>
#include "tests.h"

Suite *QuasarSuite(void);

Suite * QuasarSuite(void)
{
    Suite *s;
    s = suite_create("Quasar");
    suite_add_tcase(s, QuasarApiTCase());
    return s;
}

int main(void) {
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = QuasarSuite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
