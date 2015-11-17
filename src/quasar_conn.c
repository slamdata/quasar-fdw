/*-------------------------------------------------------------------------
 *
 * Quasar Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2015 SlamData Inc
 *
 * This software is released under the Apache 2 license
 *
 * Author: Jon Eisen <jon@joneisen.works>
 *
 * IDENTIFICATION
 *            quasar_fdw/src/conn.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "quasar_fdw.h"
#include "curl/curl.h"

#include "commands/defrem.h"
#include "utils/memutils.h"

#define INITIAL_TUPLE_ALLOC_SIZE 100;



char * execute_info_curl(QuasarConn *conn, char *url);
static size_t header_handler(void *buffer, size_t size, size_t nmemb, void *buf);
static size_t query_body_handler(void *buffer, size_t size, size_t nmemb, void *userp);
static size_t info_body_handler(void *buffer, size_t size, size_t nmemb, void *userp);
static size_t throwaway_body_handler(void *buffer, size_t size, size_t nmemb, void *userp);
static size_t count_line_endings(const char *buffer, size_t size);
void appendStringInfoQuery(CURL *curl,
                           StringInfo buf,
                           char *name,
                           const char *value,
                           bool is_first);

extern void
QuasarGlobalConnectionInit()
{
#ifdef _WIN32
    curl_global_init(CURL_GLOBAL_WIN32);
#else
    curl_global_init(CURL_GLOBAL_NOTHING);
#endif
}

/*
 * Create a connection to a server/table
 * Connections are valid for a single query/explain
 */
extern QuasarConn *
QuasarGetConnection(ForeignServer *server)
{
    ListCell *lc;
    QuasarConn *conn = palloc0(sizeof(QuasarConn));

    conn->server = DEFAULT_SERVER;
    conn->path = DEFAULT_PATH;
    conn->timeout_ms = DEFAULT_TIMEOUT_MS;

    foreach(lc, server->options)
    {
        DefElem    *def = (DefElem *) lfirst(lc);

        if (strcmp(def->defname, "server") == 0)
            conn->server = defGetString(def);
        else if (strcmp(def->defname, "path") == 0)
            conn->path = defGetString(def);
        else if (strcmp(def->defname, "timeout_ms") == 0)
            conn->timeout_ms = strtod(defGetString(def), NULL);
    }

    conn->curlm = NULL;
    conn->curl = curl_easy_init();

    conn->ongoing_transfers = 0;
    conn->exec_transfer = 0;

    conn->qctx = NULL;

    return conn;
}

extern void
QuasarPrepQuery(QuasarConn *conn, EState *estate, Relation rel)
{
    conn->curlm = curl_multi_init();

    conn->qctx = palloc0(sizeof(quasar_query_curl_context));
    conn->qctx->is_query = true;
    quasar_parse_alloc(&conn->qctx->parse, rel);
    conn->qctx->batch_count = 0;
    conn->qctx->batchmem = AllocSetContextCreate(estate->es_query_cxt,
                                                "postgres_fdw tuple data",
                                                ALLOCSET_DEFAULT_MINSIZE,
                                                ALLOCSET_DEFAULT_INITSIZE,
                                                ALLOCSET_DEFAULT_MAXSIZE);
}

extern void
QuasarCleanupConnection(QuasarConn *conn)
{
    if (conn->curlm != NULL) {
        curl_multi_remove_handle(conn->curlm, conn->curl);
        curl_multi_cleanup(conn->curlm);
    }

    curl_easy_cleanup(conn->curl);

    if (conn->qctx != NULL) {
        quasar_parse_free(&conn->qctx->parse);
        MemoryContextReset(conn->qctx->batchmem);
        pfree(conn->qctx);
    }

    pfree(conn);
}

extern void
QuasarResetConnection(QuasarConn *conn)
{
    if (conn->curlm != NULL) {
        curl_multi_remove_handle(conn->curlm, conn->curl);
    }

    curl_easy_cleanup(conn->curl);
    conn->curl = curl_easy_init();

    conn->ongoing_transfers = 0;
    conn->exec_transfer = 0;
}

extern void
QuasarExecuteQuery(QuasarConn *conn, char *query,
                   const char **param_values, size_t numParams)
{
    int sc, i;
    CURL *curl = curl_easy_init();
    CURLM *curlm = conn->curlm;
    StringInfoData url;
    StringInfoData param;
    StringInfoData dest;
    quasar_info_curl_context infoctx;
    struct curl_slist *headers = NULL;
    char *destination;

    Assert(conn->curlm != NULL && conn->qctx != NULL);

    /* Build URL */
    initStringInfo(&url);
    initStringInfo(&param);
    appendStringInfo(&url, "%s/query/fs%s", conn->server, conn->path);

    for (i = 0; i < numParams; ++i) {
        resetStringInfo(&param);
        appendStringInfo(&param, "p%d", i+1);
        appendStringInfoQuery(curl, &url, param.data, param_values[i], i == 0);
    }

    initStringInfo(&dest);
    appendStringInfo(&dest, "%s/fdw_%d", conn->path, rand());
    destination = pstrdup(dest.data);
    resetStringInfo(&dest);
    appendStringInfo(&dest, "Destination: %s", destination);
    headers = curl_slist_append(headers, dest.data);

    /* Set up CURL instance. */
    curl_easy_setopt(curl, CURLOPT_URL, url.data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_handler);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &infoctx);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, throwaway_body_handler);

    elog(DEBUG1, "curling %s with query %s", url.data, query);
    sc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (sc != CURLE_OK)
    {
        QuasarCleanupConnection(conn);
        elog(ERROR, "quasar_fdw: curl %s failed: %s", url.data, curl_easy_strerror(sc));
    }

    if (infoctx.status != 200)
    {
        QuasarCleanupConnection(conn);
        elog(ERROR, "quasar_fdw: Got bad response from quasar: %d (%s)", infoctx.status, url.data);
    }

    /* Ok now we go get the data */
    curl = conn->curl;

    resetStringInfo(&url);
    appendStringInfo(&url, "%s/data/fs%s", conn->server, destination);
    conn->full_url = url.data;

    curl_easy_setopt(curl, CURLOPT_URL, url.data);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_handler);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, conn->qctx);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, query_body_handler);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, conn->qctx);

    sc = curl_multi_add_handle(curlm, curl);
    if (sc != CURLM_OK)
    {
        QuasarCleanupConnection(conn);
        elog(ERROR, "quasar_fdw: curl add handle failed %s", curl_multi_strerror(sc));
    }

    conn->ongoing_transfers = 1;
    conn->exec_transfer = 1;
    conn->qctx->tuples = NULL;
    conn->qctx->num_tuples = 0;
    conn->qctx->next_tuple = 0;
    conn->qctx->alloc_tuples = 0;
    conn->qctx->batch_count = 0;
    conn->qctx->status = 0;

    /* Reset the parse context (for rescans) */
    quasar_parse_reset(&conn->qctx->parse);
}

extern void
QuasarContinueQuery(QuasarConn *conn) {
    int cc, nfds;
    clock_t time;

    while (conn->ongoing_transfers == 1 &&
           conn->qctx->next_tuple >= conn->qctx->num_tuples)
    {
        elog(DEBUG3, "quasar_fdw: continuing curl transfer");
        time = clock();

        /* Basically select() on curl's internal fds */
        cc = curl_multi_wait(conn->curlm, NULL, 0, conn->timeout_ms, &nfds);
        if (cc != CURLM_OK) {
            QuasarCleanupConnection(conn);
            elog(ERROR, "quasar_fdw: curl error %s", curl_multi_strerror(cc));
        }

        /* If nothing happened in timeout error out */
        if ((clock() - time) / CLOCKS_PER_SEC * 1000 >= conn->timeout_ms
            && nfds == 0) {
            QuasarCleanupConnection(conn);
            elog(ERROR, "quasar_fdw: Timeout (%ld ms) contacting quasar.", conn->timeout_ms);
        }

        /* Execute any necessary work for the request */
        cc = curl_multi_perform(conn->curlm, &conn->ongoing_transfers);
        if (cc != CURLM_OK) {
            QuasarCleanupConnection(conn);
            elog(ERROR, "quasar_fdw: curl error %s", curl_multi_strerror(cc));
        }

        /* Error out on bad status */
        if (conn->qctx->status > 0 && conn->qctx->status != 200) {
            char *url = conn->full_url;
            QuasarCleanupConnection(conn);
            elog(ERROR, "quasar_fdw: bad status from Quasar %d (%s)",
                 conn->qctx->status,  url);
        }
    }
}

extern void
QuasarRewindQuery(QuasarConn *conn)
{
    /* If we haven't done anything yet, easy! just return */
    if (conn->exec_transfer == 0)
        return;

    /* If we've only transferred a little bit, just rewind the cursor */
    if (conn->qctx->batch_count < 2) {
        conn->qctx->next_tuple = 0;
        return;
    }

    /* Otherwise, we have to reset the whole connection */
    QuasarResetConnection(conn);

}

extern char *
QuasarCompileQuery(QuasarConn *conn, char *query)
{
    StringInfoData url;

    initStringInfo(&url);
    appendStringInfo(&url, "%s/compile/fs%s", conn->server, conn->path);
    appendStringInfoQuery(conn->curl, &url, "q", query, true);

    return execute_info_curl(conn, url.data);
}

/*
 * Get a row count of the table.
 * query must be a SELECT count(*) FROM table WHERE ... clause
 */
extern double
QuasarEstimateRows(QuasarConn *conn, char *query)
{
    StringInfoData url;
    char *response;
    int n;
    long rows;

    initStringInfo(&url);
    appendStringInfo(&url, "%s/query/fs%s", conn->server, conn->path);
    appendStringInfoQuery(conn->curl, &url, "q", query, true);

    response = execute_info_curl(conn, url.data);

    /* Quasar returns no rows if nothing matched */
    if (strlen(response) == 0)
        return 0.0;

    n = sscanf(response, "{ \"0\": %ld }", &rows);

    if (n != 1)
    {
        QuasarCleanupConnection(conn);
        elog(ERROR, "Could not parse SELECT count(*) response: %s", response);
    }

    return (double) rows;
}

char *
execute_info_curl(QuasarConn *conn, char *url)
{
    int cc;
    quasar_info_curl_context ctx;
    CURL *curl = conn->curl;

    ctx.status = 0;
    initStringInfo(&ctx.buf);

    /* Set up CURL instance. */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_handler);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, info_body_handler);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    elog(DEBUG1, "quasar_fdw: Curling for information %s", url);
    cc = curl_easy_perform(curl);

    if (cc != CURLE_OK)
    {
        QuasarCleanupConnection(conn);
        elog(ERROR, "quasar_fdw: Error querying url %s %d", url, cc);
    }

    if (ctx.status != 200)
    {
        QuasarCleanupConnection(conn);
        elog(ERROR, "quasar_fdw: Bad response from Quasar %d (%s)", ctx.status, url);
    }

    return ctx.buf.data;
}

/*
 * Curl callback functions
 */

static size_t
header_handler(void *buffer, size_t size, size_t nmemb, void *userp)
{
    elog(DEBUG2, "entering function %s", __func__);
    const char *HTTP_1_1 = "HTTP/1.1";
    size_t      segsize = size * nmemb;
    quasar_info_curl_context *ctx = (quasar_info_curl_context *) userp;

    if (strncmp(buffer, HTTP_1_1, strlen(HTTP_1_1)) == 0)
    {
        int     status;

        status = atoi((char *) buffer + strlen(HTTP_1_1) + 1);
        elog(DEBUG1, "curl response status %d", status);
        ctx->status = status;
    }

    return segsize;
}

static size_t
throwaway_body_handler(void *buffer, size_t size, size_t nmemb, void *userp)
{
    return size * nmemb;
}

static size_t
info_body_handler(void *buffer, size_t size, size_t nmemb, void *userp)
{
    elog(DEBUG1, "entering function %s", __func__);
    quasar_info_curl_context *ctx = (quasar_info_curl_context *)userp;
    if (ctx->status == 200)
        appendBinaryStringInfo(&ctx->buf, buffer, size * nmemb);
    return size * nmemb;
}

static size_t
query_body_handler(void *buffer, size_t size, size_t nmemb, void *userp)
{
    elog(DEBUG3, "entering function %s", __func__);
    size_t      segsize = size * nmemb, offset = 0, new_alloc_tuples;
    quasar_query_curl_context *ctx = (quasar_query_curl_context *) userp;
    MemoryContext oldcontext;

    if (ctx->status != 200)
        return segsize;

    /* If we have reached the end of our tuple set, flush
     * otherwise, we'll continue to add on */
    if (ctx->next_tuple >= ctx->num_tuples) {
        elog(DEBUG3, "paging batched tuples %d (%d / %d)", ctx->batch_count,
             ctx->next_tuple, ctx->num_tuples);
        ctx->tuples = NULL;
        MemoryContextReset(ctx->batchmem);
        ctx->next_tuple = ctx->num_tuples = ctx->alloc_tuples = 0;
    }
    oldcontext = MemoryContextSwitchTo(ctx->batchmem);

    /* Allocate any space we need for more tuples based on a line endings
     * (over)estimate */
    new_alloc_tuples = ctx->alloc_tuples + count_line_endings(buffer, segsize) + 2;
    if (ctx->tuples == NULL) {
        ctx->tuples = palloc0(new_alloc_tuples * sizeof(HeapTuple));
        ++ctx->batch_count;
    } else {
        ctx->tuples = repalloc(ctx->tuples, new_alloc_tuples * sizeof(HeapTuple));
        memset(ctx->tuples + ctx->alloc_tuples,
               0,
               (new_alloc_tuples - ctx->alloc_tuples) * sizeof(HeapTuple));
    }
    ctx->alloc_tuples = new_alloc_tuples;


    /* Parse each tuple one at a time */
    while (offset < segsize) {
        if (quasar_parse(&ctx->parse, buffer, &offset,
                         segsize, &ctx->tuples[ctx->num_tuples])) {
            ++ctx->num_tuples;
        }
        Assert(ctx->num_tuples <= ctx->alloc_tuples);
    }

    /* Reset memory context to caller */
    MemoryContextSwitchTo(oldcontext);

    elog(DEBUG3, "Tuples prepared to be iterated over: %d / %d", ctx->next_tuple, ctx->num_tuples);

    return segsize;
}



static size_t
count_line_endings(const char *buffer, size_t size)
{
    size_t cnt = 0, left = size;
    const char *ptr = buffer;
    while (ptr != NULL)
    {
        left = size - (ptr - buffer);
        ptr = memchr(ptr, '\r', left);
        left = size - (ptr - buffer);
        if (ptr != NULL &&
            left > 1 &&
            ptr[1] == '\n')
            ++cnt;

        if (ptr != NULL) ++ptr;
    }
    return cnt;
}

/* Append a query parameter to a url buffer */
void
appendStringInfoQuery(CURL *curl,
                      StringInfo buf,
                      char *name,
                      const char *value,
                      bool is_first)
{
    char *value_encoded = curl_easy_escape(curl, value, 0);
    appendStringInfo(buf, "%s%s=%s", is_first ? "?" : "&",
                     name, value_encoded);
    curl_free(value_encoded);
}
