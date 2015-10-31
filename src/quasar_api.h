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


enum {
    API_SUCCESS = 0,
    API_FAILED
};

struct api_response {

};

int api_get(char *server, char *path, char *query, struct api_response *response);
