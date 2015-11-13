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
 *            quasar_fdw/src/quasar_fdw.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QUASAR_FDW_QUASAR_FDW_H
#define QUASAR_FDW_QUASAR_FDW_H

#include "postgres.h"
#include "foreign/foreign.h"
#include "executor/tuptable.h"
#include "lib/stringinfo.h"
#include "nodes/relation.h"
#include "utils/rel.h"

#include "yajl/yajl_parse.h"
#include <curl/curl.h>

#define BUF_SIZE 65536
#define DEFAULT_CURL_TIMEOUT_MS 1000

/*
 * Options structure to store the Quasar
 * server information
 */
typedef struct QuasarOpt
{
    char *server; /* Quasar http url */
    char *path;   /* Quasar fs path */
    char *table;  /* Quasar table name */
    long  timeout_ms; /* Timeout talking to quasar */
} QuasarOpt;


struct QuasarColumn
{
    char *name;              /* name in Quasar */
    char *pgname;            /* PostgreSQL column name */
    int pgattnum;            /* PostgreSQL attribute number */
    Oid pgtype;              /* PostgreSQL data type */
    int pgtypmod;            /* PostgreSQL type modification */
    int arrdims;             /* PostgreSQL array dimensions */
    FmgrInfo typinput;        /* Input function for pg type */
    Oid typioparam;          /* Argument for input function */
    /* int len;                 /\* element length *\/ */
    /* bool byval;              /\* element by value *\/ */
    /* char align;              /\* element alignment *\/ */
    int used;                /* is the column used in the query? */
    int warn;                /* Boolean if a warning has been issued for this col */
    bool nopushdown;         /* User option to force no pushdown of any clause
                              * with this column in it
                              * Use it when a value isn't the correct type in
                              * underlying data (such as string in mongo instead
                              * of int or date) */
};

struct QuasarTable
{
    char *name;    /* name in Quasar */
    char *pgname;  /* PostgrSQL table name */
    int ncols;     /* number of columns */
    struct QuasarColumn **cols;
};

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
    UserMapping *user;                      /* only set in use_remote_estimate mode */
} PgFdwRelationInfo;

/*
 * This is what will be set and stashed away in fdw_private and fetched
 * for subsequent routines.
 */
typedef struct QuasarFdwPlanState
{
    /* Actual query to be executed by Quasar
     * Created in createQuery
     * Passed through (de)serializePlanState
     * Used in BeginForeignScan
     */
    char *query;

    /* List of constant parameters to execute in query parameters
     * Created in createQuery
     * Passed through GetForeignRelPaths
     * Executed in BeginForeignScan
     */
    List *params;

    /* Bools representing which clauses have been fully pushed down to Quasar
     * Created by createQuery
     * Used in GetForeignRelPaths
     */
    bool *pushdown_clauses;

    /* List of pathkeys to use in this query.
     * Pathkeys are sort-order instructions
     * Created by createQuery
     * Passed to PG via GetForeignRelPaths
     */
    List *pathkeys;

    /* List of outer rels for parameterized join clauses
     * Created by createQuery
     * Passed to PG via GetForeignRelPaths
     */
    Relids outer_rels;

    /* Representation of the table we are querying */
    struct QuasarTable *quasarTable;

    long timeout_ms; /* Passing through to ExecState */
} QuasarFdwPlanState;


typedef struct quasar_parse_context
{
    yajl_handle handle;
    void *p;
} quasar_parse_context;

typedef struct quasar_curl_context {
    int status;
    StringInfoData buffer;
} quasar_curl_context;

/*
 * FDW-specific information for ForeignScanState
 * fdw_state.
 */
typedef struct QuasarFdwExecState
{
    char * url; /* for rescans */
    long timeout_ms; /* curl request timeout */

    CURLM *curlm;                    /* curl multi handle */
    CURL *curl;                      /* current transfer handle */
    int ongoing_transfers;           /* 1 if transfer still ongoing, 0 otherwise */

    quasar_curl_context *curl_ctx;   /* For buffering input data */
    size_t buffer_offset;            /* For reading bufferred input data */

    quasar_parse_context *parse_ctx; /* for parsing the data */
} QuasarFdwExecState;


/* quasar_options.c headers */
extern Datum quasar_fdw_validator(PG_FUNCTION_ARGS);
extern bool quasar_is_valid_option(const char *option, Oid context);
extern QuasarOpt *quasar_get_options(Oid foreigntableid);

/* quasar_connutil.c headers */
extern char *create_tempprefix(void);

/* quasar_parse.c headers */
void quasar_parse_alloc(quasar_parse_context *ctx, struct QuasarTable *table);
void quasar_parse_free(quasar_parse_context *ctx);
void quasar_parse_reset(quasar_parse_context *ctx);
bool quasar_parse(quasar_parse_context *ctx, const char *buffer, size_t *buf_loc, size_t buf_size);
bool quasar_parse_end(quasar_parse_context *ctx);
void quasar_parse_set_slot(quasar_parse_context *ctx, TupleTableSlot *slot);

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
                             List **retrieved_attrs);
extern void appendWhereClause(StringInfo buf,
                              PlannerInfo *root,
                              RelOptInfo *baserel,
                              List *exprs,
                              bool is_first,
                              List **params);
extern void deparseStringLiteral(StringInfo buf, const char *val);
extern Expr *find_em_expr_for_rel(EquivalenceClass *ec, RelOptInfo *rel);
extern void appendOrderByClause(StringInfo buf, PlannerInfo *root,
                                RelOptInfo *baserel, List *pathkeys);

#endif /* QUASAR_FDW_QUASAR_FDW_H */
