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
 *            quasar_fdw/src/options.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef QUASAR_FDW_QUASAR_FDW_H
#define QUASAR_FDW_QUASAR_FDW_H

#include "postgres.h"
#include "commands/copy.h"
#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "nodes/relation.h"
#include "utils/rel.h"

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

/*
 * FDW-specific information for ForeignScanState
 * fdw_state.
 */
typedef struct QuasarFdwExecState
{
    List       *copy_options;   /* merged COPY options, excluding filename */
    CopyState   cstate;         /* state of reading file */
    char       *datafn;
} QuasarFdwExecState;


/*
 * forked processes communicate via FIFO, which is described
 * in this struct. Some experiments tell that it should be
 * a bad idead to re-open these FIFO; we prepare two files
 * as one for synchronizing flag, the other for data transfer.
 */
typedef struct quasar_ipc_context
{
    char        datafn[MAXPGPATH];
    FILE       *datafp;
    char        flagfn[MAXPGPATH];
    FILE       *flagfp;
} quasar_ipc_context;

/* quasar_options.c headers */
extern Datum quasar_fdw_validator(PG_FUNCTION_ARGS);
extern bool quasar_is_valid_option(const char *option, Oid context);
extern QuasarOpt *quasar_get_options(Oid foreigntableid);

/* quasar_connutil.c headers */
extern char *create_tempprefix(void);

#endif /* QUASAR_FDW_QUASAR_FDW_H */
