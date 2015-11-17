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
 *            quasar_fdw/src/quasar_fdw.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QUASAR_FDW_QUASAR_FDW_H
#define QUASAR_FDW_QUASAR_FDW_H

#include "postgres.h"
#include "executor/tuptable.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/execnodes.h"
#include "nodes/relation.h"
#include "utils/rel.h"

#include "yajl/yajl_parse.h"
#include <curl/curl.h>

#define BUF_SIZE 65536
#define DEFAULT_CURL_TIMEOUT_MS 1000

/* Default option values */
#define DEFAULT_SERVER "http://localhost:8080"
#define DEFAULT_PATH "/test"
#define DEFAULT_TIMEOUT_MS 1000
#define DEFAULT_FDW_USE_REMOTE_ESTIMATE true
/* Cost to start up a query */
#define DEFAULT_FDW_STARTUP_COST 100.0
/* Cost to process a tuple */
#define DEFAULT_FDW_TUPLE_COST 0.01
/* Multiplier on cost to sort (~= n * log n) */
#define DEFAULT_FDW_SORT_MULTIPLIER 1.2 /* 20% */
/* ASSUME join conditions limit rowcount to 1 */
#define DEFAULT_FDW_JOIN_ROWCOUNT_ESTIMATE 1
#define QUASAR_STARTUP_COST 10.0
#define QUASAR_PER_TUPLE_COST 0.001


/*
 * FDW-specific planner information kept in RelOptInfo.fdw_private for a
 * foreign table.  This information is collected by postgresGetForeignRelSize.
 */
typedef struct QuasarFdwRelationInfo
{
    /* baserestrictinfo clauses, broken down into safe and unsafe subsets. */
    List       *remote_conds;
    List       *local_conds;

    /* Bitmap of attr numbers we need to fetch from the remote server. */
    Bitmapset  *attrs_used;

    /* Cost and selectivity of local_conds. */
    QualCost        local_conds_cost;
    Selectivity local_conds_sel;

    /* Estimated size and cost for a scan with baserestrictinfo quals. */
    double          rows;
    int                     width;
    Cost            startup_cost;
    Cost            total_cost;

    /* Options extracted from catalogs. */
    bool            use_remote_estimate;
    Cost            fdw_startup_cost;
    Cost            fdw_tuple_cost;
    List       *shippable_extensions;       /* OIDs of whitelisted extensions */

    /* Cached catalog information. */
    ForeignTable *table;
    ForeignServer *server;
} QuasarFdwRelationInfo;


typedef struct quasar_parse_context
{
    yajl_handle handle;
    void *p;
} quasar_parse_context;

typedef struct quasar_query_curl_context {
    int status;
    bool is_query;              /* True if SELECT, False if EXPLAIN */
    quasar_parse_context parse;

    MemoryContext batchmem;     /* Context for each batch of tuples */
    MemoryContext tempmem;      /* Context for temporary tuples */
    int batch_count;            /* Number of batches transferred */

    /* For converting to tuples */
    Relation rel;
    AttInMetadata *attinmeta;
    List *retrieved_attrs;

    /* for storing result tuples */
    HeapTuple  *tuples;                 /* array of currently-retrieved tuples */
    int         num_tuples;             /* # of tuples in array */
    int         next_tuple;             /* index of next one to return */
    int         alloc_tuples;           /* Number allocated spots for tuples */
} quasar_query_curl_context;

typedef struct quasar_info_curl_context {
    int status;
    StringInfoData buf;
} quasar_info_curl_context;

/*
 * FDW-specific information for ForeignScanState
 * fdw_state.
 */
typedef struct QuasarConn
{
    char *server;
    char *path;
    char *full_url;
    long timeout_ms; /* curl request timeout */

    CURLM *curlm;                    /* curl multi handle */
    CURL *curl;                      /* current transfer handle */
    int ongoing_transfers;           /* 1 if transfer still ongoing, 0 otherwise */
    int exec_transfer;               /* 1 if transfer started, 0 otherwise */

    quasar_query_curl_context *qctx;   /* For buffering tuples */
} QuasarConn;

/* quasar_conn.c headers */
extern void QuasarGlobalConnectionInit(void);
extern QuasarConn *QuasarGetConnection(ForeignServer *server);
extern void QuasarCleanupConnection(QuasarConn *conn);
extern void QuasarResetConnection(QuasarConn *conn);

extern void QuasarPrepQuery(QuasarConn *conn,
                            EState *estate,
                            Relation rel);
extern void QuasarExecuteQuery(QuasarConn *conn,
                               char *query,
                               const char **param_values,
                               size_t numParams);
extern void QuasarContinueQuery(QuasarConn *conn);
extern void QuasarRewindQuery(QuasarConn *conn);

extern double QuasarEstimateRows(QuasarConn *conn, char *query);

extern char *QuasarCompileQuery(QuasarConn *conn, char *query);

/* quasar_options.c headers */
extern Datum quasar_fdw_validator(PG_FUNCTION_ARGS);
extern bool quasar_is_valid_option(const char *option, Oid context);

/* quasar_parse.c headers */
void quasar_parse_alloc(quasar_parse_context *ctx,
                        Relation rel);
void quasar_parse_free(quasar_parse_context *ctx);
void quasar_parse_reset(quasar_parse_context *ctx);
bool quasar_parse(quasar_parse_context *ctx,
                  const char *buffer,
                  size_t *buf_loc,
                  size_t buf_size,
                  HeapTuple *result);

/* quasar_query.c headers */
extern void classifyConditions(PlannerInfo *root,
                               RelOptInfo *baserel,
                               List *input_conds,
                               List **remote_conds,
                               List **local_conds);
extern bool is_foreign_expr(PlannerInfo *root,
                            RelOptInfo *baserel,
                            Expr *expr);
extern void deparseSelectSql(StringInfo buf,
                             PlannerInfo *root,
                             RelOptInfo *baserel,
                             Bitmapset *attrs_used,
                             List **retrieved_attrs,
                             bool sizeEstimate);
extern void appendWhereClause(StringInfo buf,
                              PlannerInfo *root,
                              RelOptInfo *baserel,
                              List *exprs,
                              bool is_first,
                              List **params);
extern void deparseLiteral(StringInfo buf, Oid type, const char *svalue,
                           Datum value);
extern void deparseStringLiteral(StringInfo buf, const char *val);
extern Expr *find_em_expr_for_rel(EquivalenceClass *ec, RelOptInfo *rel);
extern void appendOrderByClause(StringInfo buf, PlannerInfo *root,
                                RelOptInfo *baserel, List *pathkeys);

#endif /* QUASAR_FDW_QUASAR_FDW_H */
