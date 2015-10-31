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
#include <stdio.h>
#include <assert.h>
#include "../../src/quasar_api.h"

int main(void) {
    struct api_response *resp = NULL;
    int res = api_get("http://localhost:8080", "/local/quasar", "select city from zips limit 3", resp);

    assert(res == API_SUCCESS);
    return 0;
}
