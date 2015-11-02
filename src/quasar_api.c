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
void free(void *ptr);
void println(char *ptr);

#include "array.h"
#include "quasar_api.h"

struct csv_udata {
    Array2D *a;
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

static void csv_cb1(void *s, size_t len, void *data) {
    struct csv_udata *u = (struct csv_udata*) data;
    if (u->linefeed) {
        array2d_linefeed(u->a);
        u->linefeed = 0;
    }

    char *field = malloc(len) + 1;
    memcpy(field, s, len);
    memset(field + len, '\0', 1);
    array2d_insert(u->a, &field);
}

static void csv_cb2(int c, void *data) {
    ((struct csv_udata *) data)->linefeed = 1;
}

void curl_csv_prepare(struct curl_csv *s) {
    csv_init(&s->p, 0);
    s->data.a = malloc(sizeof(Array2D));
    array2d_init(s->data.a, sizeof(char*), 1, 2);
    s->data.linefeed = 0;
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

void curl_csv_fini(struct curl_csv *s) {
    csv_fini(&s->p, csv_cb1, csv_cb2, &s->data);
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
    char *query_escaped = curl_easy_escape(curl, query, 0);
    int server_len = strlen(server);
    int path_len = strlen(path);
    int query_len = strlen(query_escaped);
    int url_len = server_len + 9 + path_len + 3 + query_len + 1;
    char *url = malloc(sizeof(char) * url_len);

    strcpy(url, server);
    strcpy(url + server_len, "/query/fs");
    strcpy(url + server_len + 9, path);
    strcpy(url + server_len + 9 + path_len, "?q=");
    strcpy(url + url_len - query_len - 1, query_escaped);

    curl_free(query_escaped);
    return url;
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
    array_free(r->columnNames);
    array2d_free(r->data);
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

        } else {
            curl_csv_fini(&udata);

            // inside udata.data is an Array2D with the full csv in it
            response->columnNames = array2d_pop_row(udata.data.a);
            response->data = udata.data.a;

            retval = QUASAR_API_SUCCESS;
        }

        // cleanup
        curl_easy_cleanup(curl);
        free(url);
        curl_slist_free_all(headers);
    }
    return retval;
}
