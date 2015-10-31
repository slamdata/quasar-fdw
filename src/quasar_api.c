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
void free(void *ptr);
void println(char *ptr);

#include "quasar_api.h"

struct MemoryStruct {
    char *memory;
    size_t size;
};


static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    mem->memory = realloc(mem->memory, mem->size + realsize + 1);
    if(mem->memory == NULL) {
        /* out of memory! */
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

/**
 * api_url
 * Concatentate a full url from server, path and quasar query
 * Arguments:
 *  @string server
 *  @string path
 *  @string query
 * Returns:
 *  @string url (caller is responsible for free())
 */
char *api_url(CURL *curl, char *server, char *path, char *query) {
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



void api_init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void api_cleanup() {
    curl_global_cleanup();
}

int api_get(char *server, char *path, char *query, struct api_response *response) {
    int retval = API_FAILED;
    CURL *curl = curl_easy_init();

    if (curl) {
        // Set URL
        char *url = api_url(curl, server, path, query);
        curl_easy_setopt(curl, CURLOPT_URL, url);

        // Encode with gzip (libcurl decompresses for us)
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");

        // json response
        struct curl_slist *headers=NULL;
        curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Download data to memory
        struct MemoryStruct chunk;
        chunk.memory = malloc(1);  /* will be grown as needed by the realloc above */
        chunk.size = 0;    /* no data at this point */
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        // execute
        CURLcode res = curl_easy_perform(curl);

        // Check for errors
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
                    curl_easy_strerror(res));

        } else {
            // chunk.memory points to a memory block with the full response in it
            // chunk.size is the size of the memory block

            // Parse json into api_response

            printf("I should be parsing json but instead im printing FIXME\n");
            fwrite(chunk.memory, chunk.size, 1, stdout);
            retval = API_SUCCESS;
        }

        // cleanup
        curl_easy_cleanup(curl);
        free(url);
        curl_slist_free_all(headers);
        free(chunk.memory);
    }
    return retval;
}
