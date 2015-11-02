/*-------------------------------------------------------------------------
 *
 * Quasar Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2013 Andrew Dunstan
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Andrew Dunstan <andrew@dunslane.net>
 *
 * IDENTIFICATION
 *            quasar_fdw/src/quasar_fdw.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "commands/defrem.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"

PG_MODULE_MAGIC;

/*
 * SQL functions
 */
extern Datum quasar_fdw_handler(PG_FUNCTION_ARGS);
extern Datum quasar_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(quasar_fdw_handler);
PG_FUNCTION_INFO_V1(quasar_fdw_validator);


/* callback functions */
#if (PG_VERSION_NUM >= 90200)
static void quasarGetForeignRelSize(PlannerInfo *root,
                                    RelOptInfo *baserel,
                                    Oid foreigntableid);

static void quasarGetForeignPaths(PlannerInfo *root,
                                  RelOptInfo *baserel,
                                  Oid foreigntableid);

static ForeignScan *quasarGetForeignPlan(PlannerInfo *root,
                                         RelOptInfo *baserel,
                                         Oid foreigntableid,
                                         ForeignPath *best_path,
                                         List *tlist,
                                         List *scan_clauses);
#else
static FdwPlan *quasarPlanForeignScan(Oid foreigntableid, PlannerInfo *root, RelOptInfo *baserel);
#endif

static void quasarBeginForeignScan(ForeignScanState *node,
                                      int eflags);

static TupleTableSlot *quasarIterateForeignScan(ForeignScanState *node);

static void quasarReScanForeignScan(ForeignScanState *node);

static void quasarEndForeignScan(ForeignScanState *node);


/*
 * structures used by the FDW
 *
 *
 */

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct quasarFdwOption
{
    const char *optname;
    Oid        optcontext;       /* Oid of catalog in which option may appear */
};

static struct quasarFdwOption valid_options[] =
    {
        /* Available options for CREATE SERVER */
        { "server",  ForeignServerRelationId },
        { "path",    ForeignServerRelationId },
        /* Available options for CREATE TABLE */
        { "table",   ForeignTableRelationId },
        { NULL,     InvalidOid }
    };

/*
 * This is what will be set and stashed away in fdw_private and fetched
 * for subsequent routines.
 */
typedef struct
{
      char     *foo;
      int       bar;
} QuasarFdwPlanState;


Datum
quasar_fdw_handler(PG_FUNCTION_ARGS)
{
      FdwRoutine *fdwroutine = makeNode(FdwRoutine);

      elog(DEBUG1, "entering function %s", __func__);

      /*
       * assign the handlers for the FDW
       *
       * This function might be called a number of times. In particular, it is
       * likely to be called for each INSERT statement. For an explanation, see
       * core postgres file src/optimizer/plan/createplan.c where it calls
       * GetFdwRoutineByRelId(().
       */

      /* Required by notations: S=SELECT I=INSERT U=UPDATE D=DELETE */

      /* these are required */
#if (PG_VERSION_NUM >= 90200)
      fdwroutine->GetForeignRelSize = quasarGetForeignRelSize; /* S U D */
      fdwroutine->GetForeignPaths = quasarGetForeignPaths;        /* S U D */
      fdwroutine->GetForeignPlan = quasarGetForeignPlan;          /* S U D */
#endif
      fdwroutine->BeginForeignScan = quasarBeginForeignScan;      /* S U D */
      fdwroutine->IterateForeignScan = quasarIterateForeignScan;        /* S */
      fdwroutine->ReScanForeignScan = quasarReScanForeignScan; /* S */
      fdwroutine->EndForeignScan = quasarEndForeignScan;          /* S U D */

      PG_RETURN_POINTER(fdwroutine);
}

static bool
quasarIsValidOption(const char *option, Oid context)
{
    struct quasarFdwOption *opt;

    for (opt = valid_options; opt->optname; opt++)
    {
        if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
            return true;
    }
    return false;
}

Datum
quasar_fdw_validator(PG_FUNCTION_ARGS)
{
    List     *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
    Oid       catalog = PG_GETARG_OID(1);
    ListCell  *cell;
    char      *quasar_server = NULL;
    char      *quasar_path = NULL;
    char      *quasar_table = NULL;

    elog(DEBUG1, "entering function %s", __func__);

    /* make sure the options are valid */

    foreach(cell, options_list) {
        DefElem    *def = (DefElem *) lfirst(cell);

        if (!quasarIsValidOption(def->defname, catalog)) {
            struct quasarFdwOption *opt;
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

        /*
         * Here is the code for the valid options
         */

        if (strcmp(def->defname, "server") == 0) {
            if (quasar_server)
                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("redundant options: server (%s)", defGetString(def))
                         ));

            quasar_server = defGetString(def);
        } else if (strcmp(def->defname, "path") == 0) {
            if (quasar_path)
                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("redundant options: path (%s)", defGetString(def))
                         ));

            quasar_path = defGetString(def);
        } else if (strcmp(def->defname, "table") == 0) {
            if (quasar_table)
                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("redundant options: table (%s)", defGetString(def))
                         ));

            quasar_table = defGetString(def);
        }
    }

    PG_RETURN_VOID();
}

#if (PG_VERSION_NUM >= 90200)
static void
quasarGetForeignRelSize(PlannerInfo *root,
                        RelOptInfo *baserel,
                        Oid foreigntableid)
{
      /*
       * Obtain relation size estimates for a foreign table. This is called at
       * the beginning of planning for a query that scans a foreign table. root
       * is the planner's global information about the query; baserel is the
       * planner's information about this table; and foreigntableid is the
       * pg_class OID of the foreign table. (foreigntableid could be obtained
       * from the planner data structures, but it's passed explicitly to save
       * effort.)
       *
       * This function should update baserel->rows to be the expected number of
       * rows returned by the table scan, after accounting for the filtering
       * done by the restriction quals. The initial value of baserel->rows is
       * just a constant default estimate, which should be replaced if at all
       * possible. The function may also choose to update baserel->width if it
       * can compute a better estimate of the average result row width.
       */

      QuasarFdwPlanState *fdw_private;

      elog(DEBUG1, "entering function %s", __func__);

      //baserel->rows = 0;

      fdw_private = palloc0(sizeof(QuasarFdwPlanState));
      baserel->fdw_private = (void *) fdw_private;

      /* initialize required state in fdw_private */

}

static void
quasarGetForeignPaths(PlannerInfo *root,
                      RelOptInfo *baserel,
                      Oid foreigntableid)
{
      /*
       * Create possible access paths for a scan on a foreign table. This is
       * called during query planning. The parameters are the same as for
       * GetForeignRelSize, which has already been called.
       *
       * This function must generate at least one access path (ForeignPath node)
       * for a scan on the foreign table and must call add_path to add each such
       * path to baserel->pathlist. It's recommended to use
       * create_foreignscan_path to build the ForeignPath nodes. The function
       * can generate multiple access paths, e.g., a path which has valid
       * pathkeys to represent a pre-sorted result. Each access path must
       * contain cost estimates, and can contain any FDW-private information
       * that is needed to identify the specific scan method intended.
       */

      /*
       * QuasarFdwPlanState *fdw_private = baserel->fdw_private;
       */

      Cost startup_cost, total_cost;

      elog(DEBUG1, "entering function %s", __func__);

      startup_cost = 0;
      total_cost = startup_cost + baserel->rows;

      /* Create a ForeignPath node and add it as only possible path */
      add_path(baserel, (Path *)
               create_foreignscan_path(root, baserel,
                                       baserel->rows,
                                       startup_cost,
                                       total_cost,
                                       NIL,       /* no pathkeys */
                                       NULL,            /* no outer rel either */
                                       NIL));           /* no fdw_private data */
}



static ForeignScan *
quasarGetForeignPlan(PlannerInfo *root,
                     RelOptInfo *baserel,
                     Oid foreigntableid,
                     ForeignPath *best_path,
                     List *tlist,
                     List *scan_clauses)
{
      /*
       * Create a ForeignScan plan node from the selected foreign access path.
       * This is called at the end of query planning. The parameters are as for
       * GetForeignRelSize, plus the selected ForeignPath (previously produced
       * by GetForeignPaths), the target list to be emitted by the plan node,
       * and the restriction clauses to be enforced by the plan node.
       *
       * This function must create and return a ForeignScan plan node; it's
       * recommended to use make_foreignscan to build the ForeignScan node.
       *
       */

      Index       scan_relid = baserel->relid;

      /*
       * We have no native ability to evaluate restriction clauses, so we just
       * put all the scan_clauses into the plan node's qual list for the
       * executor to check. So all we have to do here is strip RestrictInfo
       * nodes from the clauses and ignore pseudoconstants (which will be
       * handled elsewhere).
       */

      elog(DEBUG1, "entering function %s", __func__);

      scan_clauses = extract_actual_clauses(scan_clauses, false);

      /* Create the ForeignScan node */
#if(PG_VERSION_NUM < 90500)
      return make_foreignscan(tlist,
                              scan_clauses,
                              scan_relid,
                              NIL,  /* no expressions to evaluate */
                              NIL);       /* no private state either */
#else
      return make_foreignscan(tlist,
                              scan_clauses,
                              scan_relid,
                              NIL,  /* no expressions to evaluate */
                              NIL,  /* no private state either */
                              NIL);       /* no private state either */
#endif

}

#endif


static void
quasarBeginForeignScan(ForeignScanState *node,
                       int eflags)
{
      /*
       * Begin executing a foreign scan. This is called during executor startup.
       * It should perform any initialization needed before the scan can start,
       * but not start executing the actual scan (that should be done upon the
       * first call to IterateForeignScan). The ForeignScanState node has
       * already been created, but its fdw_state field is still NULL.
       * Information about the table to scan is accessible through the
       * ForeignScanState node (in particular, from the underlying ForeignScan
       * plan node, which contains any FDW-private information provided by
       * GetForeignPlan). eflags contains flag bits describing the executor's
       * operating mode for this plan node.
       *
       * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this function
       * should not perform any externally-visible actions; it should only do
       * the minimum required to make the node state valid for
       * ExplainForeignScan and EndForeignScan.
       *
       */

      elog(DEBUG1, "entering function %s", __func__);

}


static TupleTableSlot *
quasarIterateForeignScan(ForeignScanState *node)
{
      /*
       * Fetch one row from the foreign source, returning it in a tuple table
       * slot (the node's ScanTupleSlot should be used for this purpose). Return
       * NULL if no more rows are available. The tuple table slot infrastructure
       * allows either a physical or virtual tuple to be returned; in most cases
       * the latter choice is preferable from a performance standpoint. Note
       * that this is called in a short-lived memory context that will be reset
       * between invocations. Create a memory context in BeginForeignScan if you
       * need longer-lived storage, or use the es_query_cxt of the node's
       * EState.
       *
       * The rows returned must match the column signature of the foreign table
       * being scanned. If you choose to optimize away fetching columns that are
       * not needed, you should insert nulls in those column positions.
       *
       * Note that PostgreSQL's executor doesn't care whether the rows returned
       * violate any NOT NULL constraints that were defined on the foreign table
       * columns — but the planner does care, and may optimize queries
       * incorrectly if NULL values are present in a column declared not to
       * contain them. If a NULL value is encountered when the user has declared
       * that none should be present, it may be appropriate to raise an error
       * (just as you would need to do in the case of a data type mismatch).
       */


      /*
       * QuasarFdwExecutionState *festate = (QuasarFdwExecutionState *)
       * node->fdw_state;
       */
      TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

      elog(DEBUG1, "entering function %s", __func__);

      ExecClearTuple(slot);

      /* get the next record, if any, and fill in the slot */

      /* then return the slot */
      return slot;
}


static void
quasarReScanForeignScan(ForeignScanState *node)
{
      /*
       * Restart the scan from the beginning. Note that any parameters the scan
       * depends on may have changed value, so the new scan does not necessarily
       * return exactly the same rows.
       */

      elog(DEBUG1, "entering function %s", __func__);

}


static void
quasarEndForeignScan(ForeignScanState *node)
{
      /*
       * End the scan and release resources. It is normally not important to
       * release palloc'd memory, but for example open files and connections to
       * remote servers should be cleaned up.
       */

      elog(DEBUG1, "entering function %s", __func__);

}
