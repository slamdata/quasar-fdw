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
 *		  quasar_fdw/src/quasar_api.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QUASAR_FDW_QUASAR_API_H
#define QUASAR_FDW_QUASAR_API_H

#include "postgres.h"
#include "nodes/pg_list.h"

enum {
    QUASAR_API_SUCCESS = 0,
    QUASAR_API_FAILED
};

typedef struct QuasarApiResponse {
    /* A list of string column names */
    List *columnNames;
    /* A list of lists of values */
    List *data;
} QuasarApiResponse;

void quasar_api_init(void);
void quasar_api_cleanup(void);
void quasar_api_response_free(QuasarApiResponse *response);
int quasar_api_get(char *server, char *path, char *query, QuasarApiResponse *response);

#endif /* QUASAR_FDW_QUASAR_API_H */
