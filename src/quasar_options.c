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

#include "postgres.h"

#include "quasar_fdw.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "funcapi.h"
#include "access/reloptions.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "optimizer/cost.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"

#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/planmain.h"

#define DEFAULT_SERVER "http://localhost:8080"
#define DEFAULT_PATH "/test"

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct QuasarFdwOption
{
    const char *optname;
    Oid optcontext; /* Oid of catalog in which option may appear */
};


/*
 * Valid options for quasar_fdw.
 *
 */
static struct QuasarFdwOption valid_options[] =
{
    /* Available options for CREATE SERVER */
    { "server",  ForeignServerRelationId },
    { "path",    ForeignServerRelationId },
    { "timeout_ms", ForeignServerRelationId },
    /* Available options for CREATE FOREIGN TABLE */
    { "table",   ForeignTableRelationId },
    /* Available options for columns inside CREATE FOREIGN TABLE */
    { "map",     AttributeRelationId },
    { "nopushdown", AttributeRelationId },
    /* Sentinel */
    { NULL,     InvalidOid }
};

extern Datum quasar_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(quasar_fdw_validator);


/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses file_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
Datum
quasar_fdw_validator(PG_FUNCTION_ARGS)
{
    List        *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
    Oid         catalog = PG_GETARG_OID(1);
    ListCell    *cell;

    /*
     * Check that only options supported by quasar_fdw,
     * and allowed for the current object type, are given.
     */
    foreach(cell, options_list)
    {
        DefElem  *def = (DefElem *) lfirst(cell);

        if (!quasar_is_valid_option(def->defname, catalog))
        {
            struct QuasarFdwOption *opt;
            StringInfoData buf;

            /*
             * Unknown option specified, complain about it. Provide a hint
             * with list of valid options for the object.
             */
            initStringInfo(&buf);
            for (opt = valid_options; opt->optname; opt++)
            {
                if (catalog == opt->optcontext)
                    appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
                             opt->optname);
            }

            ereport(ERROR,
                (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                errmsg("invalid option \"%s\"", def->defname),
                 errhint("Valid options in this context are: %s", buf.len ? buf.data : "<none>")
                ));
        }
    }
    PG_RETURN_VOID();
}


/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
bool
quasar_is_valid_option(const char *option, Oid context)
{
    struct QuasarFdwOption *opt;

    for (opt = valid_options; opt->optname; opt++)
    {
        if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
            return true;
    }
    return false;
}

/*
 * Fetch the options for a quasar_fdw foreign table.
 */
QuasarOpt* quasar_get_options(Oid foreignoid) {
    ForeignTable *f_table = NULL;
    ForeignServer *f_server = NULL;
    /* UserMapping *f_mapping; */
    List *options;
    ListCell *lc;
    QuasarOpt *opt;

    opt = (QuasarOpt*) palloc(sizeof(QuasarOpt));
    memset(opt, 0, sizeof(QuasarOpt));

    /*
     * Extract options from FDW objects.
     */
    PG_TRY();
    {
        f_table = GetForeignTable(foreignoid);
        f_server = GetForeignServer(f_table->serverid);
    }
    PG_CATCH();
    {
        f_table = NULL;
        f_server = GetForeignServer(foreignoid);
    }
    PG_END_TRY();

    /* f_mapping = GetUserMapping(GetUserId(), f_server->serverid); */

    options = NIL;
    if (f_table)
        options = list_concat(options, f_table->options);
    options = list_concat(options, f_server->options);
    /* options = list_concat(options, f_mapping->options); */

    /* Defaults */
    opt->server = DEFAULT_SERVER;
    opt->path = DEFAULT_PATH;
    opt->timeout_ms = DEFAULT_CURL_TIMEOUT_MS;

    /* Loop through the options, and get the server/port */
    foreach(lc, options)
    {
        DefElem *def = (DefElem *) lfirst(lc);

        if (strcmp(def->defname, "server") == 0)
            opt->server = defGetString(def);
        else if (strcmp(def->defname, "path") == 0)
            opt->path = defGetString(def);
        else if (strcmp(def->defname, "timeout_ms") == 0)
            opt->timeout_ms = atol(defGetString(def));
        else if (strcmp(def->defname, "table") == 0)
            opt->table = defGetString(def);
    }

    if (opt->timeout_ms <= 0) {
        elog(ERROR, "Option timeout_ms requires a positive value");
    }

    return opt;
}
