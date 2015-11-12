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

#define BUF_SIZE 65536

/*
 * Options structure to store the Quasar
 * server information
 */
typedef struct QuasarOpt
{
    char *server; /* Quasar http url */
    char *path;   /* Quasar fs path */
    char *table;  /* Quasar table name */
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
};

struct QuasarTable
{
    char *name;    /* name in Quasar */
    char *pgname;  /* PostgrSQL table name */
    int ncols;     /* number of columns */
    struct QuasarColumn **cols;
};

/*
 * This is what will be set and stashed away in fdw_private and fetched
 * for subsequent routines.
 */
typedef struct QuasarFdwPlanState
{
    /* Actual query to be executed by Quasar */
    char *query;

    /* List of constant parameters to execute in query parameters */
    List *params;

    /* Bools representing which clauses have been fully pushed down to Quasar */
    bool *pushdown_clauses;

    /* Representation of the table we are querying */
    struct QuasarTable *quasarTable;
} QuasarFdwPlanState;


typedef struct quasar_parse_context
{
    yajl_handle handle;
    void *p;
} quasar_parse_context;

/*
 * FDW-specific information for ForeignScanState
 * fdw_state.
 */
typedef struct QuasarFdwExecState
{
    /* for rescans */
    char * url;
    /* for saving the buffer of raw curl data */
    char * buffer;
    size_t buf_loc;
    size_t buf_size;
    quasar_parse_context *parse_ctx; /* for parsing the data */
} QuasarFdwExecState;

typedef struct quasar_curl_context {
    int status;
    StringInfoData buffer;
} quasar_curl_context;

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

#endif /* QUASAR_FDW_QUASAR_FDW_H */
