/*-------------------------------------------------------------------------
 *
 * Quasar Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2015 SlamData Inc
 *
 * This software is released under the Apache 2 License
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
    { "use_remote_estimate", ForeignServerRelationId },
    { "fdw_startup_cost", ForeignServerRelationId },
    { "fdw_tuple_cost", ForeignServerRelationId },
        /* Available options for CREATE FOREIGN TABLE */
    { "table",   ForeignTableRelationId },
    { "use_remote_estimate", ForeignTableRelationId },
    /* Available options for columns inside CREATE FOREIGN TABLE */
    { "map",     AttributeRelationId },
    { "nopushdown", AttributeRelationId },
    { "join_rowcount_estimate", AttributeRelationId },
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
