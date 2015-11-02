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
 *		  quasar_fdw/src/quasar_api.c
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <curl/curl.h>
#include <csv.h>

#include "postgres.h"
#include "nodes/pg_list.h"
#include "lib/stringinfo.h"
#include "quasar_api.h"


struct csv_udata {
    List *a;
    int linefeed;
};

/**
 * Wrap up a csv parser and quasar api response
 * Used as user space data for curl callbacks
 */
struct curl_csv {
    struct csv_parser p;
    struct csv_udata data;
};

/* Internal functions for this file */
char * quasar_api_url(CURL *curl, char *server, char *path, char *query);
static void csv_cb1(void *s, size_t len, void *data);
static void csv_cb2(int c, void *data);
void curl_csv_prepare(struct curl_csv *s);
void curl_csv_abort_cleanup(struct curl_csv *s);
static size_t curl_csv_callback(void *contents, size_t size, size_t nmemb, void *userp);
void curl_csv_fini(struct curl_csv *s, QuasarApiResponse *r);



static void csv_cb1(void *s, size_t len, void *data) {
    struct csv_udata *u = (struct csv_udata*) data;
    if (u->linefeed) {
        lappend(u->a, NIL);
        u->linefeed = 0;
    }

    char *field = palloc(len + 1);
    memcpy(field, s, len);
    memset(field + len, '\0', 1);
    lappend(llast(u->a), field);
}

static void csv_cb2(int c, void *data) {
    ((struct csv_udata *) data)->linefeed = 1;
}

void curl_csv_prepare(struct curl_csv *s) {
    csv_init(&s->p, 0);
    s->data.a = NIL;
    s->data.linefeed = 1;
}

void curl_csv_abort_cleanup(struct curl_csv *s) {
    ListCell *lc;

    csv_free(&s->p);
    foreach(lc, s->data.a) {
        list_free_deep(lfirst(lc));
    }
    list_free(s->data.a);
}

/**
 * curl response callback to parse csv into quasar_api_response
 */
static size_t curl_csv_callback(
    void *contents, size_t size, size_t nmemb, void *userp)
{
    struct curl_csv *s = (struct curl_csv*) userp;
    return csv_parse(&s->p, contents, size * nmemb, csv_cb1, csv_cb2, &s->data);
}

void curl_csv_fini(struct curl_csv *s, QuasarApiResponse *r) {
    csv_fini(&s->p, csv_cb1, csv_cb2, &s->data);
    r->columnNames = lfirst(list_head(s->data.a));
    r->data = list_delete_first(s->data.a);
}

/**
 * quasar_api_url
 * Concatentate a full url from server, path and quasar query
 * Arguments:
 *  @string server
 *  @string path
 *  @string query
 * Returns:
 *  @string url (caller is responsible for free())
 */
char *quasar_api_url(CURL *curl, char *server, char *path, char *query) {
    StringInfoData buf;
    initStringInfo(&buf);
    char *query_escaped = curl_easy_escape(curl, query, 0);
    appendStringInfo(&buf, "%s/query/fs/%s?q=%s", server, path, query_escaped);

    curl_free(query_escaped);
    return buf.data;
}



void quasar_api_init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void quasar_api_cleanup() {
    curl_global_cleanup();
}

/**
 * quasar_api_response_free
 * Free the memory of an quasar_api_response object
 * Arguments:
 *  @QuasarApiResponse * response
 */
void quasar_api_response_free(QuasarApiResponse *r) {
    ListCell *lc;
    list_free_deep(r->columnNames);
    foreach(lc, r->data) {
        list_free_deep(lfirst(lc));
    }
    list_free(r->data);
    r->columnNames = r->data = NULL;
}


int quasar_api_get(char *server, char *path, char *query, QuasarApiResponse *response) {
    int retval = QUASAR_API_FAILED;
    CURL *curl = curl_easy_init();

    if (curl) {
        // Set URL
        char *url = quasar_api_url(curl, server, path, query);
        curl_easy_setopt(curl, CURLOPT_URL, url);

        // Encode with gzip (libcurl decompresses for us)
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");

        // json response
        struct curl_slist *headers=NULL;
        headers = curl_slist_append(headers, "Accept: text/csv");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Create user data structure to store csv
        struct curl_csv udata;
        curl_csv_prepare(&udata);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_csv_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&udata);

        // execute
        CURLcode res = curl_easy_perform(curl);

        // Check for errors
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
                    curl_easy_strerror(res));
            curl_csv_abort_cleanup(&udata);

        } else {
            curl_csv_fini(&udata, response);
            retval = QUASAR_API_SUCCESS;
        }

        // cleanup
        curl_easy_cleanup(curl);
        pfree(url);
        curl_slist_free_all(headers);
    }
    return retval;
}
