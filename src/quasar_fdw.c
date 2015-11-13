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
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <yajl/yajl_parse.h>

#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "libpq/md5.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port.h"
#include "postmaster/fork_process.h"
#include "storage/ipc.h"
#include "storage/fd.h"
#include "storage/lock.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/elog.h"
#include "utils/fmgroids.h"
#include "utils/formatting.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"
#include "utils/tqual.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include <string.h>
#include <stdlib.h>

#include <curl/curl.h>

#include "quasar_fdw.h"

PG_MODULE_MAGIC;

/*
 * SQL functions
 */
extern Datum quasar_fdw_handler(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(quasar_fdw_handler);

/*
 * on-load initializer
 */
extern PGDLLEXPORT void _PG_init(void);


/* callback functions */
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

static void quasarBeginForeignScan(ForeignScanState *node,
                                      int eflags);

static TupleTableSlot *quasarIterateForeignScan(ForeignScanState *node);

static void quasarReScanForeignScan(ForeignScanState *node);

static void quasarEndForeignScan(ForeignScanState *node);

static void quasarExplainForeignScan(ForeignScanState *node, ExplainState *es);


/*
 * Curl functions
 */
void cleanup_curl(struct QuasarFdwExecState *festate);
void execute_curl(struct QuasarFdwExecState *festate);
static size_t header_handler(void *buffer, size_t size, size_t nmemb, void *buf);
static size_t body_handler(void *buffer, size_t size, size_t nmemb, void *userp);

/*
 * Private functions
 */
void createQuery(PlannerInfo *root, RelOptInfo *foreignrel, struct QuasarTable *quasarTable, QuasarFdwPlanState *plan);
struct QuasarTable *getQuasarTable(Oid foreigntableid, QuasarOpt *opt);
void getUsedColumns(Expr *expr, struct QuasarTable *quasarTable);
char *getQuasarClause(RelOptInfo *foreignrel, Expr *expr, const struct QuasarTable *quasarTable, List **params);
List *serializePlanState(struct QuasarFdwPlanState *fdwState);
struct QuasarFdwPlanState *deserializePlanState(List *l);
static char *datumToString(Datum datum, Oid type);
Expr *find_em_expr_for_rel(EquivalenceClass *ec, RelOptInfo *rel);

/*
 * _PG_init
 *      Library load-time initalization.
 *      Sets exitHook() callback for backend shutdown.
 *      Also finds the OIDs of PostGIS the PostGIS geometry type.
 */
void
_PG_init(void)
{
#ifdef _WIN32
    curl_global_init(CURL_GLOBAL_WIN32);
#else
    curl_global_init(CURL_GLOBAL_NOTHING);
#endif
}

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
      fdwroutine->GetForeignRelSize = quasarGetForeignRelSize; /* S U D */
      fdwroutine->GetForeignPaths = quasarGetForeignPaths;        /* S U D */
      fdwroutine->GetForeignPlan = quasarGetForeignPlan;          /* S U D */
      fdwroutine->BeginForeignScan = quasarBeginForeignScan;      /* S U D */
      fdwroutine->IterateForeignScan = quasarIterateForeignScan;        /* S */
      fdwroutine->ReScanForeignScan = quasarReScanForeignScan; /* S */
      fdwroutine->EndForeignScan = quasarEndForeignScan;          /* S U D */
      fdwroutine->ExplainForeignScan = quasarExplainForeignScan; /* E */

      PG_RETURN_POINTER(fdwroutine);
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

      QuasarFdwPlanState *fdwPlan;

      elog(DEBUG1, "entering function %s", __func__);

      baserel->rows = 0;

      fdwPlan = palloc0(sizeof(QuasarFdwPlanState));
      baserel->fdw_private = (void *) fdwPlan;
      fdwPlan->params = NIL;

      /* initialize required state in fdw_private */

      QuasarOpt *opt = quasar_get_options(foreigntableid);
      struct QuasarTable *qTable = getQuasarTable(foreigntableid, opt);
      createQuery(root, baserel, qTable, fdwPlan);
      fdwPlan->quasarTable = qTable;
      fdwPlan->timeout_ms = opt->timeout_ms;
      elog(DEBUG1, "quasar_fdw: query is %s", fdwPlan->query);
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

      QuasarFdwPlanState *fdwPlan = baserel->fdw_private;

      Cost startup_cost, total_cost;

      elog(DEBUG1, "entering function %s", __func__);

      startup_cost = 100;
      total_cost = startup_cost;

      elog(DEBUG1, "join length %d", list_length(baserel->joininfo));
      elog(DEBUG1, "query pathkeys %d", list_length(root->query_pathkeys));


      /* Create a ForeignPath node and add it as only possible path */
      add_path(baserel, (Path *)
               create_foreignscan_path(root, baserel,
                                       baserel->rows,
                                       startup_cost,
                                       total_cost,
                                       fdwPlan->pathkeys,
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

      elog(DEBUG1, "entering function %s", __func__);

      QuasarFdwPlanState *fdwPlan = (QuasarFdwPlanState*) baserel->fdw_private;
      List * fdw_private = serializePlanState(fdwPlan);
      List *keep_clauses = NIL;
      ListCell *cell1, *cell2;
      int i;
      List *params = fdwPlan->params;

      /* keep only those clauses that are not handled by Quasar */
      foreach(cell1, scan_clauses)
      {
          i = 0;
          foreach(cell2, baserel->baserestrictinfo)
          {
              if (equal(lfirst(cell1), lfirst(cell2)) && ! fdwPlan->pushdown_clauses[i])
              {
                  keep_clauses = lcons(lfirst(cell1), keep_clauses);
                  break;
              }
              ++i;
          }
      }

      keep_clauses = extract_actual_clauses(keep_clauses, false);


      pfree(fdwPlan);
      baserel->fdw_private = NULL;

      /* Create the ForeignScan node */
#if(PG_VERSION_NUM < 90500)
      elog(DEBUG1, "make_foreignscan %d", baserel->relid);
      return make_foreignscan(tlist,
                              keep_clauses,
                              baserel->relid,
                              params,
                              fdw_private);
#else
      return make_foreignscan(tlist,
                              keep_clauses,
                              baserel->relid,
                              params,
                              fdw_private,
                              NIL);
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

      /*
       * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
       */
      if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
          return;

      ForeignScan *fsplan = (ForeignScan *)node->ss.ps.plan;
      QuasarFdwExecState *festate;
      QuasarOpt *opt;
      StringInfoData buf;
      char       *query_encoded;
      ExprContext *econtext;
      MemoryContext oldcontext;
      List *exec_params;
      ListCell *cell;
      int index;
      struct QuasarFdwPlanState *plan = deserializePlanState((List*) ((ForeignScan*)node->ss.ps.plan)->fdw_private);
      CURL *curl;

      festate = (QuasarFdwExecState *) palloc0(sizeof(QuasarFdwExecState));
      festate->parse_ctx = palloc0(sizeof(quasar_parse_context));
      quasar_parse_alloc(festate->parse_ctx, plan->quasarTable);

      /* Setup curl */
      curl = festate->curl = curl_easy_init();
      festate->curlm = curl_multi_init();
      festate->timeout_ms = plan->timeout_ms;

      /* Fetch options of foreign table */
      opt = quasar_get_options(RelationGetRelid(node->ss.ss_currentRelation));

      /* Encode query */
      query_encoded = curl_easy_escape(curl, plan->query, 0);
      elog(DEBUG1, "encoding query %s -> %s", plan->query, query_encoded);
      pfree(plan->query);

      /* Make the url */
      initStringInfo(&buf);
      appendStringInfo(&buf, "%s/query/fs%s?q=%s",
                       opt->server, opt->path, query_encoded);
      curl_free(query_encoded);

      /* Execute any exec params and include in the query as quasar params */
      econtext = node->ss.ps.ps_ExprContext;
      /* switch to short lived memory context */
      oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
      index = 0;
      exec_params = (List *)ExecInitExpr((Expr *)fsplan->fdw_exprs,
                                         (PlanState *)node);
      foreach(cell, exec_params) {
          ExprState *expr = (ExprState *)lfirst(cell);
          bool is_null;

          /* count, but skip deleted entries */
          ++index;
          if (expr == NULL)
              continue;

          /* Evaluate the expression */
          Datum datum = ExecEvalExpr((ExprState *)expr, econtext, &is_null, NULL);

          /* Check for null */
          if (is_null) {
              appendStringInfo(&buf, "&p%d=null", index);
          } else {
              Oid type;
              regproc typoutput;
              bool typ_isvarlena;
              FmgrInfo typoutput_info;
              char *value, *value_encoded;

              /* Get type of expression */
              type = exprType((Node *)(expr->expr));
              /* Get output function reference */
              getTypeOutputInfo(type, &typoutput, &typ_isvarlena);
              /* Convert output function reference to FmgrInfo */
              fmgr_info(typoutput, &typoutput_info);
              /* Call output function on eval'ed expression */
              value = OutputFunctionCall(&typoutput_info, datum);

              /* Escape the value for putting in a query string */
              value_encoded = curl_easy_escape(curl, value, 0);

              /* Insert into the query string */
              appendStringInfo(&buf, "&p%d=%s", index, value_encoded);

              curl_free(value_encoded); /* Cleanup of malloc'd memory. */
              /* We don't have to clean up palloc'd memory
                 (thats why we went into short-lived memory) */
          }
      }
      /* reset memory context */
      MemoryContextSwitchTo(oldcontext);

      festate->url = buf.data;

      /* cleanup after making url */
      pfree(plan);

      /* Execute the curl */
      execute_curl(festate);

      node->fdw_state = (void *) festate;
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

    elog(DEBUG3, "entering function %s", __func__);

    QuasarFdwExecState *festate = (QuasarFdwExecState *) node->fdw_state;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
    int nfds;
    bool finished_transfer = false;
    int cc;
    clock_t time;
    long timeout_ms = festate->timeout_ms;

    /*
     * The protocol for loading a virtual tuple into a slot is first
     * ExecClearTuple, then fill the values/isnull arrays, then
     * ExecStoreVirtualTuple.  If we don't find another row in the file, we
     * just skip the last step, leaving the slot empty as required.
     *
     */

    ExecClearTuple(slot);
    quasar_parse_set_slot(festate->parse_ctx, slot);

    for (;;) {

        /* Pull down more data when we don't have data */
        if (festate->ongoing_transfers == 1
            && festate->buffer_offset == festate->curl_ctx->buffer.len) {
            elog(DEBUG3, "quasar_fdw: continuing curl transfer (timeout_ms %ld)", timeout_ms);
            time = clock();

            /* Basically select() on curl's internal fds */
            cc = curl_multi_wait(festate->curlm, NULL, 0, timeout_ms, &nfds);
            if (cc != CURLM_OK) {
                cleanup_curl(festate);
                elog(ERROR, "quasar_fdw: curl error %s", curl_multi_strerror(cc));
            }

            /* If nothing happened in timeout (after a first perform),
               error out */
            if ((clock() - time) / CLOCKS_PER_SEC * 1000 >= timeout_ms
                && nfds == 0) {
                cleanup_curl(festate);
                elog(ERROR, "quasar_fdw: Timeout (%ld ms) contacting quasar.", timeout_ms);
            }

            /* Execute any necessary work for the request */
            cc = curl_multi_perform(festate->curlm, &festate->ongoing_transfers);
            if (cc != CURLM_OK) {
                cleanup_curl(festate);
                elog(ERROR, "quasar_fdw: curl error %s", curl_multi_strerror(cc));
            }
        } else if (finished_transfer) {
            /* If we have finished the transfer and tried to parse another row
             * but failed to find a tuple, we know that there are no more. */
            return slot;
        } else {
            finished_transfer = true;
        }

        /* Parse any new buffer available */
        if (festate->buffer_offset < festate->curl_ctx->buffer.len) {
            /* Parse a row and store it if we find one */
            if (quasar_parse(festate->parse_ctx,
                             festate->curl_ctx->buffer.data,
                             &festate->buffer_offset,
                             festate->curl_ctx->buffer.len)) {
                ExecStoreVirtualTuple(slot);
                return slot;
            }
        }

    }

    /* Shouldn't get here */
    return slot;
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

      QuasarFdwExecState *festate = (QuasarFdwExecState *) node->fdw_state;
      /* if festate is NULL, we are in EXPLAIN; nothing to do */
      if (festate) {
          cleanup_curl(festate);

          quasar_parse_free(festate->parse_ctx);
          pfree(festate->parse_ctx);
          pfree(festate->curl_ctx->buffer.data);
          pfree(festate->curl_ctx);
          pfree(festate->url);
          pfree(festate);
      }
}

 static void
     quasarReScanForeignScan(ForeignScanState *node) {
    /*
     * Restart the scan from the beginning. Note that any parameters the scan
     * depends on may have changed value, so the new scan does not necessarily
     * return exactly the same rows.
     */

    elog(DEBUG1, "entering function %s", __func__);
    QuasarFdwExecState *festate = (QuasarFdwExecState *) node->fdw_state;

    /* Stop current request */
    curl_multi_remove_handle(festate->curlm, festate->curl);
    curl_easy_cleanup(festate->curl);

    /* Start a new request */
    festate->curl = curl_easy_init();
    execute_curl(festate);
 }


/*
 * quasarExplainForeignScan
 *              Produce extra output for EXPLAIN:
 *              the Quasar query
 */
static void quasarExplainForeignScan(ForeignScanState *node, ExplainState *es) {
    struct QuasarFdwPlanState *plan = deserializePlanState((List*) ((ForeignScan*)node->ss.ps.plan)->fdw_private);

    elog(DEBUG1, "Entering function %s", __func__);

    elog(DEBUG1, "quasar_fdw: explain foreign table scan on %d", RelationGetRelid(node->ss.ss_currentRelation));

    /* show query */
    ExplainPropertyText("Quasar query", plan->query, es);
}

void cleanup_curl(struct QuasarFdwExecState *festate) {
    curl_multi_remove_handle(festate->curlm, festate->curl);
    curl_easy_cleanup(festate->curl);
    curl_multi_cleanup(festate->curlm);
}

void
execute_curl(struct QuasarFdwExecState *festate) {
    int sc;
    CURL *curl = festate->curl;
    CURLM *curlm = festate->curlm;

    if (festate->curl_ctx == NULL) {
        festate->curl_ctx = palloc0(sizeof(quasar_curl_context));
        initStringInfo(&festate->curl_ctx->buffer);
    } else {
        festate->curl_ctx->status = 0;
        resetStringInfo(&festate->curl_ctx->buffer);
    }

    /* Set up CURL instance. */
    curl_easy_setopt(curl, CURLOPT_URL, festate->url);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_handler);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, festate->curl_ctx);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_handler);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, festate->curl_ctx);

    elog(DEBUG1, "curling %s", festate->url);
    sc = curl_multi_add_handle(curlm, curl);
    if (sc != 0) {
        /* Cleanup before exception */
        cleanup_curl(festate);
        elog(ERROR, "curl %s failed: %d", festate->url, sc);
    }

    festate->buffer_offset = 0;
    festate->ongoing_transfers = 1;

    /* Reset the parse context (for rescans) */
    quasar_parse_reset(festate->parse_ctx);
}

/*
 * Curl callback functions
 */

static size_t
header_handler(void *buffer, size_t size, size_t nmemb, void *userp)
{
    elog(DEBUG1, "entering function %s", __func__);
    const char *HTTP_1_1 = "HTTP/1.1";
    size_t      segsize = size * nmemb;
    quasar_curl_context *ctx = (quasar_curl_context *) userp;

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
body_handler(void *buffer, size_t size, size_t nmemb, void *userp)
{
    elog(DEBUG3, "entering function %s", __func__);
    size_t      segsize = size * nmemb;
    quasar_curl_context *ctx = (quasar_curl_context *) userp;

    if (ctx->status == 200) {
        appendBinaryStringInfo(&ctx->buffer, buffer, segsize);
    }
    elog(DEBUG3, "wrote %ld bytes to curl buffer", segsize);

    return segsize;
}


/* serializePlanState
 * serialize the Plan state in a List
 *
 * Notably, pushdown_clauses is not serialize because it isn't needed
 */
List *serializePlanState(struct QuasarFdwPlanState *fdwState) {
    int i;
    List *l = list_make2(makeString(fdwState->query),
                         makeInteger(fdwState->timeout_ms));

    l = lappend(l, makeString(fdwState->quasarTable->name));
    l = lappend(l, makeString(fdwState->quasarTable->pgname));
    l = lappend(l, makeInteger(fdwState->quasarTable->ncols));
    for (i=0; i<fdwState->quasarTable->ncols; ++i) {
        struct QuasarColumn *col = fdwState->quasarTable->cols[i];
        l = lappend(l, makeString(col->name));
        l = lappend(l, makeString(col->pgname));
        l = lappend(l, makeInteger(col->pgattnum));
        l = lappend(l, makeInteger(col->pgtype));
        l = lappend(l, makeInteger(col->pgtypmod));
        l = lappend(l, makeInteger(col->arrdims));
        l = lappend(l, makeInteger(col->used));
    }

    return l;
}

#define lcpop(fld) ((Value*)lfirst(lc))->val.fld; lc = lnext(lc)

/* deserializePlanState
 * deserialize a list into a plan state
 *
 * Notably, pushdown_clauses is not deserialized because it isn't needed.
 */
struct QuasarFdwPlanState *deserializePlanState(List *l) {
    ListCell *lc = list_head(l);
    int i;

    struct QuasarFdwPlanState *state = palloc0(sizeof(struct QuasarFdwPlanState));
    state->quasarTable = palloc0(sizeof(struct QuasarTable));

    state->query = lcpop(str);
    state->timeout_ms = lcpop(ival);
    state->quasarTable->name = lcpop(str);
    state->quasarTable->pgname = lcpop(str);
    state->quasarTable->ncols = lcpop(ival);

    state->quasarTable->cols = palloc0(state->quasarTable->ncols *
                                       sizeof(struct QuasarColumn *));
    for (i = 0; i < state->quasarTable->ncols; i++) {
        struct QuasarColumn *col = palloc0(sizeof(struct QuasarColumn));
        state->quasarTable->cols[i] = col;

        col->name = lcpop(str);
        col->pgname = lcpop(str);
        col->pgattnum = lcpop(ival);
        col->pgtype = (Oid) lcpop(ival);
        col->pgtypmod = lcpop(ival);
        col->arrdims = lcpop(ival);
        col->used = lcpop(ival);

        /* Fill out typinput and typioparam fields */
        Oid in_func_oid;
        getTypeInputInfo(col->pgtype, &in_func_oid, &col->typioparam);
        fmgr_info(in_func_oid, &col->typinput);
    }

    return state;
}


/* getQuasarTable
 *     Retrieve information on a QuasarTable
 */
struct QuasarTable *getQuasarTable(Oid foreigntableid, QuasarOpt *opt) {
    Relation rel;
    TupleDesc tupdesc;
    int i, index;

    elog(DEBUG1, "entering function %s", __func__);

    struct QuasarTable *table = palloc(sizeof(struct QuasarTable));
    table->pgname = get_rel_name(foreigntableid);
    table->name = opt->table;

    /* Grab the foreign table relation from the PostgreSQL catalog */
    rel = heap_open(foreigntableid, NoLock);
    tupdesc = rel->rd_att;

    /* number of PostgreSQL columns */
    table->ncols = tupdesc->natts;
    elog(DEBUG1, "quasar_fdw table: pgname %s name %s ncols %d", table->pgname, table->name, table->ncols);

    table->cols = palloc(table->ncols * sizeof(struct QuasarColumn *));

    /* loop through foreign table columns */
    index = 0;
    for (i=0; i<tupdesc->natts; ++i)
    {
        Form_pg_attribute att_tuple = tupdesc->attrs[i];
        List *options;
        ListCell *option;

        /* ignore dropped columns */
        if (att_tuple->attisdropped)
            continue;

        ++index;
        /* get PostgreSQL column number and type */
        if (index <= table->ncols)
        {
            struct QuasarColumn *col = table->cols[index-1] = palloc0(sizeof(struct QuasarColumn));
            col->pgattnum = att_tuple->attnum;
            col->pgtype = att_tuple->atttypid;
            col->pgtypmod = att_tuple->atttypmod;
            col->arrdims = att_tuple->attndims;
            col->pgname = pstrdup(NameStr(att_tuple->attname));
            col->name = NULL;

            /* loop through column options */
            options = GetForeignColumnOptions(foreigntableid, att_tuple->attnum);
            foreach(option, options)
            {
                DefElem *def = (DefElem *)lfirst(option);

                /* Allow users to map column names */
                if (strcmp(def->defname, "map") == 0)
                {
                    col->name = defGetString(def);
                }

                if (strcmp(def->defname, "nopushdown") == 0) {
                    col->nopushdown = true;
                }
            }

            if (col->name == NULL) {
                col->name = pstrdup(col->pgname);
            }

            elog(DEBUG1, "quasar_fdw column: pgname %s name %s index %dnum %d atttypid %d addtypmod %d", col->pgname, col->name, index-1, col->pgattnum, col->pgtype, col->pgtypmod);
        }
    }

    heap_close(rel, NoLock);

    return table;
}

/*
 * createQuery
 *      Construct a query string for Quasar that
 *      a) contains only the necessary columns in the SELECT list
 *      b) has all the WHERE clauses that can safely be translated to Quasar.
 *      Untranslatable WHERE clauses are omitted and left for PostgreSQL to check.
 *      In "pushdown_clauses" an array is stored that contains "true" for all clauses
 *      that will be pushed down and "false" for those that are filtered locally.
 *      As a side effect, we also mark the used columns in quasarTable.
 */
void createQuery(PlannerInfo *root, RelOptInfo *foreignrel, struct QuasarTable *quasarTable, QuasarFdwPlanState *plan)
{
    ListCell *cell;
    bool first_col = true, in_quote = false;
    int i, clause_count = -1, index;
    StringInfoData query;
    char *where, parname[10], *p;
    List *columnlist = foreignrel->reltargetlist,
        *conditions = foreignrel->baserestrictinfo;
    struct QuasarColumn *col;
    List **params = &plan->params;
    bool **pushdown_clauses = &plan->pushdown_clauses;
    List **usable_pathkeys = &plan->pathkeys;

    elog(DEBUG1, "entering function %s", __func__);

    /* first, find all the columns to include in the select list */

    /* examine each SELECT list entry for Var nodes */
    foreach(cell, columnlist)
    {
        getUsedColumns((Expr *)lfirst(cell), quasarTable);
    }

    /* examine each condition for Var nodes */
    foreach(cell, conditions)
    {
        getUsedColumns((Expr *)lfirst(cell), quasarTable);
    }

    /* construct SELECT list */
    initStringInfo(&query);
    appendStringInfo(&query, "SELECT ");
    for (i=0; i<quasarTable->ncols; ++i)
    {
        col = quasarTable->cols[i];
        if (col->used)
        {
            if (!first_col)
            {
                appendStringInfo(&query, ", ");
            } else {
                first_col = false;
            }
            if (strcmp(col->name, col->pgname) != 0) {
                appendStringInfo(&query, "%s AS %s", col->name, col->pgname);
            } else {
                appendStringInfo(&query, "%s", col->name);
            }
        }
    }

    /* dummy column if there is no result column we need from Quasar */
    if (first_col)
        appendStringInfo(&query, "'1'");
    appendStringInfo(&query, " FROM %s", quasarTable->name);


    /* allocate enough space for pushdown_clauses */
    if (conditions != NIL)
    {
        *pushdown_clauses = (bool *)palloc(sizeof(bool) * list_length(conditions));
    }

    /* append WHERE clauses */
    first_col = true;
    foreach(cell, conditions)
    {
        /* try to convert each condition to Oracle SQL */
        where = getQuasarClause(foreignrel, ((RestrictInfo *)lfirst(cell))->clause, quasarTable, params);
        if (where != NULL) {
            /* append new WHERE clause to query string */
            if (first_col)
            {
                first_col = false;
                appendStringInfo(&query, " WHERE %s", where);
            }
            else
            {
                appendStringInfo(&query, " AND %s", where);
            }
            pfree(where);

            (*pushdown_clauses)[++clause_count] = true;
        }
        else
            (*pushdown_clauses)[++clause_count] = false;
    }

    /*
     * Push down ORDER BY and other sort clauses, known as "pathkeys" to Postgres
     * This will avoid a local sort in PG.
     */

    StringInfoData orderby;
    initStringInfo(&orderby);
    first_col = true;
    foreach(cell, root->query_pathkeys) {
        PathKey    *pathkey = (PathKey *) lfirst(cell);
        EquivalenceClass *pathkey_ec = pathkey->pk_eclass;
        Expr       *em_expr;
        char *querypart = NULL;

        elog(DEBUG1, "Checking query_pathkey");

        /*
         * getQuasarClause would detect volatile expressions as well, but
         * ec_has_volatile saves some cycles.
         */
        if (!pathkey_ec->ec_has_volatile) {
            em_expr = find_em_expr_for_rel(pathkey_ec, foreignrel);
            if (em_expr) {
                querypart = getQuasarClause(foreignrel, em_expr, quasarTable, params);
            }
        }

        if (querypart != NULL) {
            if (!first_col) appendStringInfoChar(&orderby, ',');
            first_col = false;
            appendStringInfo(&orderby, "%s", querypart);
            *usable_pathkeys = lappend(*usable_pathkeys, pathkey);
        } else {
            /*
             * The planner and executor don't have any clever strategy for
             * taking data sorted by a prefix of the query's pathkeys and
             * getting it to be sorted by all of those pathekeys.  We'll just
             * end up resorting the entire data set.  So, unless we can push
             * down all of the query pathkeys, forget it.
             */
            list_free(*usable_pathkeys);
            *usable_pathkeys = NIL;
            resetStringInfo(&orderby);
            break;
        }
    }
    if (*usable_pathkeys != NIL) {
        appendStringInfo(&query, " ORDER BY %s", orderby.data);
    }
    pfree(orderby.data);

    plan->query = pstrdup(query.data);

    /* Remove string literals from this copy of the query */
    for (p=query.data; *p!='\0'; ++p)
    {
        if (*p == '\'')
            in_quote = ! in_quote;
        if (in_quote)
            *p = ' ';
    }

    /* remove all parameters that do not actually occur in the query */
    index = 0;
    foreach(cell, *params)
    {
        ++index;
        snprintf(parname, 10, ":p%d", index);
        if (strstr(query.data, parname) == NULL)
        {
            /* set the element to NULL to indicate it's gone */
            lfirst(cell) = NULL;
        }
    }

    pfree(query.data);
}

/*
 * getUsedColumns
 *      Set "used=true" in quasarTable for all columns used in the expression.
 */
void getUsedColumns(Expr *expr, struct QuasarTable *quasarTable)
{
    ListCell *cell;
    Var *variable;
    int index;

    elog(DEBUG1, "entering function %s", __func__);

    if (expr == NULL)
        return;

    switch(expr->type)
    {
        case T_RestrictInfo:
            elog(DEBUG2, "getUsedColumns: T_RestrictInfo");
            getUsedColumns(((RestrictInfo *)expr)->clause, quasarTable);
            break;
        case T_TargetEntry:
            elog(DEBUG2, "getUsedColumns: T_TargetEntry");
            getUsedColumns(((TargetEntry *)expr)->expr, quasarTable);
            break;
        case T_Const:
            elog(DEBUG2, "getUsedColumns: T_Const");
            break;
        case T_Param:
            elog(DEBUG2, "getUsedColumns: T_Param");
            break;
        case T_CaseTestExpr:
            elog(DEBUG2, "getUsedColumns: T_CaseTestExpr");
            break;
        case T_CoerceToDomainValue:
            elog(DEBUG2, "getUsedColumns: T_CoerceToDomainValue");
            break;
        case T_CurrentOfExpr:
            elog(DEBUG2, "getUsedColumns: T_CurrentOfExpr");
            break;
        case T_Var:
            elog(DEBUG2, "getUsedColumns: T_Var");
            variable = (Var *)expr;

            /* ignore system columns */
            if (variable->varattno < 1)
                break;

            /* get quasarTable column index corresponding to this column (-1 if none) */
            index = quasarTable->ncols - 1;
            while (index >= 0 && quasarTable->cols[index]->pgattnum != variable->varattno)
                --index;

            if (index == -1)
            {
                ereport(WARNING,
                        (errcode(ERRCODE_WARNING),
                        errmsg("column number %d of foreign table \"%s\" does not exist in foreign Quasar table, will be replaced by NULL", variable->varattno, quasarTable->pgname)));
            }
            else
            {
                elog(DEBUG1, "quasar_fdw: column %s is used in statement", quasarTable->cols[index]->name);
                quasarTable->cols[index]->used = 1;
            }
            break;
        case T_Aggref:
            elog(DEBUG2, "getUsedColumns: T_Aggref");
            foreach(cell, ((Aggref *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            foreach(cell, ((Aggref *)expr)->aggorder)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            foreach(cell, ((Aggref *)expr)->aggdistinct)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_WindowFunc:
            elog(DEBUG2, "getUsedColumns: T_WindowFunc");
            foreach(cell, ((WindowFunc *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_ArrayRef:
            elog(DEBUG2, "getUsedColumns: T_ArrayRef");
            foreach(cell, ((ArrayRef *)expr)->refupperindexpr)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            foreach(cell, ((ArrayRef *)expr)->reflowerindexpr)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            getUsedColumns(((ArrayRef *)expr)->refexpr, quasarTable);
            getUsedColumns(((ArrayRef *)expr)->refassgnexpr, quasarTable);
            break;
        case T_FuncExpr:
            elog(DEBUG2, "getUsedColumns: T_FuncExpr");
            foreach(cell, ((FuncExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_OpExpr:
            elog(DEBUG2, "getUsedColumns: T_OpExpr");
            foreach(cell, ((OpExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_DistinctExpr:
            elog(DEBUG2, "getUsedColumns: T_DistinctExpr");
            foreach(cell, ((DistinctExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_NullIfExpr:
            elog(DEBUG2, "getUsedColumns: T_NullIfExpr");
            foreach(cell, ((NullIfExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_ScalarArrayOpExpr:
            elog(DEBUG2, "getUsedColumns: T_ScalaArrayOpExpr");
            foreach(cell, ((ScalarArrayOpExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_BoolExpr:
            elog(DEBUG2, "getUsedColumns: T_BoolExpr");
            foreach(cell, ((BoolExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_SubPlan:
            elog(DEBUG2, "getUsedColumns: T_SubPlan");
            foreach(cell, ((SubPlan *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_AlternativeSubPlan:
            elog(DEBUG2, "getUsedColumns: T_AlternativeSubPlan");
            /* examine only first alternative */
            getUsedColumns((Expr *)linitial(((AlternativeSubPlan *)expr)->subplans), quasarTable);
            break;
        case T_NamedArgExpr:
            elog(DEBUG2, "getUsedColumns: T_NamedArgExpr");
            getUsedColumns(((NamedArgExpr *)expr)->arg, quasarTable);
            break;
        case T_FieldSelect:
            elog(DEBUG2, "getUsedColumns: T_FieldSelect");
            getUsedColumns(((FieldSelect *)expr)->arg, quasarTable);
            break;
        case T_RelabelType:
            elog(DEBUG2, "getUsedColumns: T_RelabelType");
            getUsedColumns(((RelabelType *)expr)->arg, quasarTable);
            break;
        case T_CoerceViaIO:
            elog(DEBUG2, "getUsedColumns: T_CoerceViaIO");
            getUsedColumns(((CoerceViaIO *)expr)->arg, quasarTable);
            break;
        case T_ArrayCoerceExpr:
            elog(DEBUG2, "getUsedColumns: T_ArrayCoerceExpr");
            getUsedColumns(((ArrayCoerceExpr *)expr)->arg, quasarTable);
            break;
        case T_ConvertRowtypeExpr:
            elog(DEBUG2, "getUsedColumns: T_ConvertRowtypeExpr");
            getUsedColumns(((ConvertRowtypeExpr *)expr)->arg, quasarTable);
            break;
        case T_CollateExpr:
            elog(DEBUG2, "getUsedColumns: T_CollateExpr");
            getUsedColumns(((CollateExpr *)expr)->arg, quasarTable);
            break;
        case T_CaseExpr:
            elog(DEBUG2, "getUsedColumns: T_CaseExpr");
            foreach(cell, ((CaseExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            getUsedColumns(((CaseExpr *)expr)->arg, quasarTable);
            getUsedColumns(((CaseExpr *)expr)->defresult, quasarTable);
            break;
        case T_CaseWhen:
            elog(DEBUG2, "getUsedColumns: T_CaseWhen");
            getUsedColumns(((CaseWhen *)expr)->expr, quasarTable);
            getUsedColumns(((CaseWhen *)expr)->result, quasarTable);
            break;
        case T_ArrayExpr:
            elog(DEBUG2, "getUsedColumns: T_ArrayExpr");
            foreach(cell, ((ArrayExpr *)expr)->elements)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_RowExpr:
            elog(DEBUG2, "getUsedColumns: T_RowExpr");
            foreach(cell, ((RowExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_RowCompareExpr:
            elog(DEBUG2, "getUsedColumns: T_RowCompareExpr");
            foreach(cell, ((RowCompareExpr *)expr)->largs)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            foreach(cell, ((RowCompareExpr *)expr)->rargs)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_CoalesceExpr:
            elog(DEBUG2, "getUsedColumns: T_CoalesceExpr");
            foreach(cell, ((CoalesceExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_MinMaxExpr:
            elog(DEBUG2, "getUsedColumns: T_MinMaxExpr");
            foreach(cell, ((MinMaxExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_XmlExpr:
            elog(DEBUG2, "getUsedColumns: T_XmlExpr");
            foreach(cell, ((XmlExpr *)expr)->named_args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            foreach(cell, ((XmlExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_NullTest:
            elog(DEBUG2, "getUsedColumns: T_NullTest");
            getUsedColumns(((NullTest *)expr)->arg, quasarTable);
            break;
        case T_BooleanTest:
            elog(DEBUG2, "getUsedColumns: T_BooleanTest");
            getUsedColumns(((BooleanTest *)expr)->arg, quasarTable);
            break;
        case T_CoerceToDomain:
            elog(DEBUG2, "getUsedColumns: T_CoerceToDomain");
            getUsedColumns(((CoerceToDomain *)expr)->arg, quasarTable);
            break;
        default:
            /*
             * We must be able to handle all node types that can
             * appear because we cannot omit a column from the remote
             * query that will be needed.
             * Throw an error if we encounter an unexpected node type.
             */
            ereport(ERROR,
                    (errcode(ERRCODE_FDW_UNABLE_TO_CREATE_REPLY),
                    errmsg("Internal quasar_fdw error: encountered unknown node type %d.", expr->type)));
    }
}


/*
 * datumToString
 *      Convert a Datum to a string by calling the type output function.
 *      Returns the result or NULL if it cannot be converted to Quasar SQL.
 */
static char
*datumToString(Datum datum, Oid type)
{
    StringInfoData result;
    regproc typoutput;
    HeapTuple tuple;
    char *str, *p;

    /* get the type's output function */
    tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
    if (!HeapTupleIsValid(tuple))
    {
        elog(ERROR, "%s: cache lookup failed for type %u", __func__, type);
    }
    typoutput = ((Form_pg_type)GETSTRUCT(tuple))->typoutput;
    ReleaseSysCache(tuple);

    /* render the constant in Quasar SQL */
    switch (type)
    {
        case TEXTOID:
        case CHAROID:
        case BPCHAROID:
        case VARCHAROID:
        case NAMEOID:
            str = DatumGetCString(OidFunctionCall1(typoutput, datum));

            /* quote string */
            initStringInfo(&result);
            appendStringInfo(&result, "'");
            for (p=str; *p; ++p)
            {
                if (*p == '\'')
                    appendStringInfo(&result, "'");
                appendStringInfo(&result, "%c", *p);
            }
            appendStringInfo(&result, "'");
            break;
        case INT8OID:
        case INT2OID:
        case INT4OID:
        case OIDOID:
        case FLOAT4OID:
        case FLOAT8OID:
        case NUMERICOID:
            str = DatumGetCString(OidFunctionCall1(typoutput, datum));
            initStringInfo(&result);
            appendStringInfo(&result, "%s", str);
            break;
        case DATEOID:
            str = DatumGetCString(OidFunctionCall1(typoutput, datum));
            initStringInfo(&result);
            appendStringInfo(&result, "DATE '%s'", str);
            break;
        case TIMESTAMPOID:
            str = DatumGetCString(OidFunctionCall1(typoutput, datum));
            initStringInfo(&result);
            appendStringInfo(&result, "TIMESTAMP '%s'", str);
            break;
        /* case TIMESTAMPTZOID: */
        case INTERVALOID:
            str = DatumGetCString(OidFunctionCall1(typoutput, datum));
            initStringInfo(&result);
            appendStringInfo(&result, "INTERVAL '%s'", str);
            break;
        default:
            elog(DEBUG2, "quasar_fdw: dont know how to get cstring from %d", type);
            return NULL;
    }

    return result.data;
}

/*
 * This macro is used by getQuasarClause to identify PostgreSQL
 * types that can be translated to Quasar SQL-2.
 */
#define canHandleType(x) ((x) == TEXTOID || (x) == CHAROID || (x) == BPCHAROID \
                          || (x) == VARCHAROID || (x) == NAMEOID || (x) == INT8OID || (x) == INT2OID \
                          || (x) == INT4OID || (x) == OIDOID || (x) == FLOAT4OID || (x) == FLOAT8OID \
                          || (x) == NUMERICOID || (x) == DATEOID || (x) == TIMEOID || (x) == TIMESTAMPOID \
                          || (x) == INTERVALOID || (x) == BOOLOID)

/* macro used by getQuasarClause
 * Recursively calls getQuasarClause on an expr
 * and checks it for NULL, returns NULL after freeing things
 */
#define renderFree2(name, expr, free1, free2)                           \
    name = getQuasarClause(foreignrel, (expr), quasarTable, params);    \
    if (name == NULL) {                                                 \
        if ((free1) != NULL) pfree((free1));                            \
        if ((free2) != NULL) pfree((free2));                            \
        return NULL;                                                    \
    }
#define render(name, expr) renderFree2(name, expr, NULL, NULL);
#define renderFree(name, expr, free) renderFree2(name, expr, free, NULL);

/*
 * getQuasarClause
 *      Create an Quasar SQL WHERE clause from the expression and store in in "where".
 *      Returns NULL if that is not possible, else a palloc'ed string.
 *      As a side effect, all Params incorporated in the WHERE clause
 *      will be stored in paramList.
 */
char *
getQuasarClause(RelOptInfo *foreignrel, Expr *expr, const struct QuasarTable *quasarTable, List **params)
{
    char *opername, *left, *right, *arg, oprkind;
    Const *constant;
    OpExpr *oper;
    ScalarArrayOpExpr *arrayoper;
    BoolExpr *boolexpr;
    Param *param;
    Var *variable;
    FuncExpr *func;
    regproc typoutput;
    HeapTuple tuple;
    ListCell *cell;
    StringInfoData result;
    Oid leftargtype, rightargtype, schema;
    ArrayIterator iterator;
    Datum datum;
    bool first_arg, isNull;
    int index;

    if (expr == NULL)
        return NULL;

    switch(expr->type)
    {
    case T_Const:
        elog(DEBUG2, "getQuasarClause: T_Const");
        constant = (Const *)expr;
        if (constant->constisnull)
        {
            /* only translate NULLs of a type Quasar can handle */
            if (canHandleType(constant->consttype))
            {
                initStringInfo(&result);
                appendStringInfo(&result, "NULL");
            }
            else
                return NULL;
        }
        else
        {
            /* get a string representation of the value */
            char *c = datumToString(constant->constvalue, constant->consttype);
            if (c == NULL)
                return NULL;
            else
            {
                initStringInfo(&result);
                appendStringInfo(&result, "%s", c);
                pfree(c);
            }
        }
        break;
    case T_Param:
        elog(DEBUG2, "getQuasarClause: T_Param");
        param = (Param *)expr;

        if (! canHandleType(param->paramtype))
            return NULL;

        /* find the index in the parameter list */
        index = 0;
        foreach(cell, *params)
        {
            ++index;
            if (equal(param, (Node *)lfirst(cell)))
                break;
        }
        if (cell == NULL)
        {
            /* add the parameter to the list */
            ++index;
            *params = lappend(*params, param);
        }

        /* parameters will be called :p1, :p2 etc. */
        initStringInfo(&result);
        appendStringInfo(&result, ":p%d", index);

        break;

    case T_Var:
        elog(DEBUG2, "getQuasarClause: T_Var");
        variable = (Var *)expr;

        if (variable->varno == foreignrel->relid && variable->varlevelsup == 0)
        {
            /* the variable belongs to the foreign table, replace it with the name */

            /* we cannot handle system columns */
            if (variable->varattno < 1)
                return NULL;

            /*
             * Allow boolean columns here.
             * They will be rendered as ("COL" <> 0).
             */
            if (! canHandleType(variable->vartype))
                return NULL;

            /* get quasarTable column index corresponding to this column (-1 if none) */
            index = quasarTable->ncols - 1;
            while (index >= 0 && quasarTable->cols[index]->pgattnum != variable->varattno)
                --index;

            /* if no Quasar column corresponds, translate as NULL */
            if (index == -1)
            {
                initStringInfo(&result);
                appendStringInfo(&result, "NULL");
                break;
            }

            /* Abandon pushdown if user told us not to pushdown this column */
            if (quasarTable->cols[index]->nopushdown)
                return NULL;

            initStringInfo(&result);
            appendStringInfo(&result, "%s", quasarTable->cols[index]->name);

        }
        else
        {
            /* treat it like a parameter */

            if (! canHandleType(variable->vartype))
                return NULL;

            /* find the index in the parameter list */
            index = 0;
            foreach(cell, *params)
            {
                ++index;
                if (equal(variable, (Node *)lfirst(cell)))
                    break;
            }
            if (cell == NULL)
            {
                /* add the parameter to the list */
                ++index;
                *params = lappend(*params, variable);
            }

            /* parameters will be called :p1, :p2 etc. */
            initStringInfo(&result);
            appendStringInfo(&result, ":p%d", index);
        }

        break;
    case T_OpExpr:
        elog(DEBUG2, "getQuasarClause: T_OpExpr");
        oper = (OpExpr *)expr;

        /* get operator name, kind, argument type and schema */
        tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(oper->opno));
        if (! HeapTupleIsValid(tuple))
        {
            elog(ERROR, "cache lookup failed for operator %u", oper->opno);
        }
        opername = pstrdup(((Form_pg_operator)GETSTRUCT(tuple))->oprname.data);
        oprkind = ((Form_pg_operator)GETSTRUCT(tuple))->oprkind;
        leftargtype = ((Form_pg_operator)GETSTRUCT(tuple))->oprleft;
        rightargtype = ((Form_pg_operator)GETSTRUCT(tuple))->oprright;
        schema = ((Form_pg_operator)GETSTRUCT(tuple))->oprnamespace;
        ReleaseSysCache(tuple);

        /* ignore operators in other than the pg_catalog schema */
        if (schema != PG_CATALOG_NAMESPACE)
            return NULL;

        if (! canHandleType(rightargtype))
            return NULL;

        /* OPERATORS */
        /* the operators that we can translate */
        if (strcmp(opername, "=") == 0
            || strcmp(opername, "<>") == 0
            || strcmp(opername, ">") == 0
            || strcmp(opername, "<") == 0
            || strcmp(opername, ">=") == 0
            || strcmp(opername, "<=") == 0
            || strcmp(opername, "+") == 0
            || strcmp(opername, "/") == 0
            /* Cannot subtract DATEs in Quasar */
            || (strcmp(opername, "-") == 0 && rightargtype != DATEOID
                && rightargtype != TIMESTAMPOID
                && rightargtype != TIMESTAMPTZOID)
            || strcmp(opername, "*") == 0
            /* Regexes in Quasar only take constant right sides */
            || (strcmp(opername, "~~") == 0 && ((Expr*)lsecond(oper->args))->type == T_Const)
            || (strcmp(opername, "!~~") == 0 && ((Expr*)lsecond(oper->args))->type == T_Const)
            /* || strcmp(opername, "~~*") == 0 */ /* Case Insensitive LIKE */
            /* || strcmp(opername, "!~~*") == 0 */ /* Not Case Insensitive LIKE */
            /* || strcmp(opername, "^") == 0 */ /* Power */
            || strcmp(opername, "%") == 0
            /* || strcmp(opername, "&") == 0 */ /* Bit-and */
            /* || strcmp(opername, "|/") == 0 */ /* Square Root */
            /* || strcmp(opername, "@") == 0 */ /* Absolute Value */
            )
        {
            renderFree(left, linitial(oper->args), opername);

            if (oprkind == 'b')
            {
                /* TRANSFORM BINARY OPERATOR */
                renderFree2(right, lsecond(oper->args), left, opername);

                initStringInfo(&result);
                if (strcmp(opername, "~~") == 0)
                {
                    /* TODO Add ESCAPE args to LIKE? */
                    appendStringInfo(&result, "(%s LIKE %s)", left, right);
                }
                else if (strcmp(opername, "!~~") == 0)
                {
                    appendStringInfo(&result, "(%s NOT LIKE %s)", left, right);
                }
                else
                {
                    /* the other operators have the same name in Quasar */
                    appendStringInfo(&result, "(%s %s %s)", left, opername, right);
                }
                pfree(right);
                pfree(left);
            }
            else
            {
                /* TRANSFORM UNARY OPERATOR */
                /* unary operator */
                initStringInfo(&result);
                /* unary + or - */
                appendStringInfo(&result, "(%s%s)", opername, left);

                pfree(left);
            }
        }
        else
        {
            /* cannot translate this operator */
            pfree(opername);
            return NULL;
        }

        pfree(opername);
        break;
    case T_ScalarArrayOpExpr:
        elog(DEBUG2, "getQuasarClause: T_ScalarArrayOpExpr");
        arrayoper = (ScalarArrayOpExpr *)expr;

        /* get operator name, left argument type and schema */
        tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(arrayoper->opno));
        if (! HeapTupleIsValid(tuple))
        {
            elog(ERROR, "cache lookup failed for operator %u", arrayoper->opno);
        }
        opername = pstrdup(((Form_pg_operator)GETSTRUCT(tuple))->oprname.data);
        leftargtype = ((Form_pg_operator)GETSTRUCT(tuple))->oprleft;
        schema = ((Form_pg_operator)GETSTRUCT(tuple))->oprnamespace;
        ReleaseSysCache(tuple);

        /* get the type's output function */
        tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(leftargtype));
        if (!HeapTupleIsValid(tuple))
        {
            elog(ERROR, "%s cache lookup failed for type %u", __func__, leftargtype);
        }
        typoutput = ((Form_pg_type)GETSTRUCT(tuple))->typoutput;
        ReleaseSysCache(tuple);

        /* ignore operators in other than the pg_catalog schema */
        if (schema != PG_CATALOG_NAMESPACE)
            return NULL;

        /* don't try to push down anything but IN and NOT IN expressions */
        if ((strcmp(opername, "=") != 0 || ! arrayoper->useOr)
            && (strcmp(opername, "<>") != 0 || arrayoper->useOr))
            return NULL;

        if (! canHandleType(leftargtype))
            return NULL;

        render(left, linitial(arrayoper->args));

        /* only push down IN expressions with constant second (=last) argument */
        if (((Expr *)llast(arrayoper->args))->type != T_Const)
            return NULL;

        /* begin to compose result */
        initStringInfo(&result);
        appendStringInfo(&result, "(%s %s [", left, arrayoper->useOr ? "IN" : "NOT IN");

        /* the second (=last) argument must be a Const of ArrayType */
        constant = (Const *)llast(arrayoper->args);

        /* using NULL in place of an array or value list is valid in Quasar and PostgreSQL */
        if (constant->constisnull)
            appendStringInfo(&result, "NULL");
        else
        {
            /* loop through the array elements */
            iterator = array_create_iterator(DatumGetArrayTypeP(constant->constvalue), 0);
            first_arg = true;
            while (array_iterate(iterator, &datum, &isNull))
            {
                char *c;

                if (isNull)
                    c = "NULL";
                else
                {
                    c = datumToString(datum, leftargtype);
                    if (c == NULL)
                    {
                        array_free_iterator(iterator);
                        return NULL;
                    }
                }

                /* append the argument */
                appendStringInfo(&result, "%s%s", first_arg ? "" : ", ", c);
                first_arg = false;
            }
            array_free_iterator(iterator);

            /* don't push down empty arrays, since the semantics for NOT x = ANY(<empty array>) differ */
            if (first_arg)
                return NULL;
        }

        /* two parentheses close the expression */
        appendStringInfo(&result, "])");

        break;
    case T_DistinctExpr:        /* (x IS DISTINCT FROM y) */
        elog(DEBUG2, "getQuasarClause: T_DistinctExpr");
        return NULL;
        break;
    case T_NullIfExpr:          /* NULLIF(x, y) */
        elog(DEBUG2, "getQuasarClause: T_NullIfExpr");
        return NULL;
        break;
    case T_BoolExpr:
        elog(DEBUG2, "getQuasarClause: T_BoolExpr");
        boolexpr = (BoolExpr *)expr;

        arg = getQuasarClause(foreignrel, linitial(boolexpr->args), quasarTable, params);
        if (arg == NULL)
            return NULL;

        initStringInfo(&result);
        appendStringInfo(&result, "(%s%s",
                         boolexpr->boolop == NOT_EXPR ? "NOT " : "",
                         arg);

        for_each_cell(cell, lnext(list_head(boolexpr->args)))
        {
            renderFree(arg, (Expr*) lfirst(cell), result.data);

            appendStringInfo(&result, " %s %s",
                             boolexpr->boolop == AND_EXPR ? "AND" : "OR",
                             arg);
        }
        appendStringInfo(&result, ")");

        break;
    case T_RelabelType:
        elog(DEBUG2, "getQuasarClause: T_RelabelType");
        return getQuasarClause(foreignrel, ((RelabelType *)expr)->arg, quasarTable, params);
        break;
    case T_CoerceToDomain:
        elog(DEBUG2, "getQuasarClause: T_CoerceToDomain");
        return getQuasarClause(foreignrel, ((CoerceToDomain *)expr)->arg, quasarTable, params);
        break;
    case T_CaseExpr:
        elog(DEBUG2, "getQuasarClause: T_CaseExpr");
        return NULL;
        break;
    case T_CoalesceExpr:
        elog(DEBUG2, "getQuasarClause: T_CoalesceExpr");
        return NULL;
        break;
    case T_NullTest:
        elog(DEBUG2, "getQuasarClause: T_NullTest");
        render(arg, ((NullTest *)expr)->arg);

        initStringInfo(&result);
        appendStringInfo(&result, "(%s IS %sNULL)",
                         arg,
                         ((NullTest *)expr)->nulltesttype == IS_NOT_NULL ? "NOT " : "");
        break;
    case T_FuncExpr:
        elog(DEBUG2, "getQuasarClause: T_FuncExpr");
        func = (FuncExpr *)expr;

        if (! canHandleType(func->funcresulttype))
            return NULL;

        /* do nothing for implicit casts */
        if (func->funcformat == COERCE_IMPLICIT_CAST)
            return getQuasarClause(foreignrel, linitial(func->args), quasarTable, params);

        /* get function name and schema */
        tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(func->funcid));
        if (! HeapTupleIsValid(tuple))
        {
            elog(ERROR, "cache lookup failed for function %u", func->funcid);
        }
        opername = pstrdup(((Form_pg_proc)GETSTRUCT(tuple))->proname.data);
        schema = ((Form_pg_proc)GETSTRUCT(tuple))->pronamespace;
        ReleaseSysCache(tuple);

        /* ignore functions in other than the pg_catalog schema */
        if (schema != PG_CATALOG_NAMESPACE)
            return NULL;

        /* FUNCTIONS */
        /* the "normal" functions that we can translate */
        if (strcmp(opername, "char_length") == 0
            || strcmp(opername, "character_length") == 0
            || strcmp(opername, "concat") == 0
            || strcmp(opername, "length") == 0
            || strcmp(opername, "lower") == 0
            || (strcmp(opername, "substr") == 0 && list_length(func->args) == 3)
            || (strcmp(opername, "substring") == 0 && list_length(func->args) == 3)
            || strcmp(opername, "upper") == 0
            || strcmp(opername, "date_part") == 0
            || strcmp(opername, "to_timestamp") == 0)
        {
            initStringInfo(&result);

            /* FUNCTION TRANSFORMS */
            if (strcmp(opername, "char_length") == 0 ||
                strcmp(opername, "character_length") == 0)
                appendStringInfo(&result, "length(");
            else if (strcmp(opername, "substr") == 0)
                appendStringInfo(&result, "substring(");
            else
                appendStringInfo(&result, "%s(", opername);

            first_arg = true;
            foreach(cell, func->args)
            {
                renderFree2(arg, lfirst(cell), result.data, opername);

                if (!first_arg) appendStringInfoChar(&result, ',');
                appendStringInfo(&result, "%s", arg);
                first_arg = false;

                pfree(arg);
            }

            appendStringInfoChar(&result, ')');
        }
        else
        {
            /* function that we cannot render for Quasar */
            pfree(opername);
            return NULL;
        }

        pfree(opername);
        break;
    case T_CoerceViaIO:
        /*
         * Quasar doesn't support CAST
         */
        elog(DEBUG2, "getQuasarClause: T_CoerceViaIO");
        return NULL;
        break;
    default:
        /* we cannot translate this to Quasar */
        elog(DEBUG2, "getQuasarClause: Other: %d", expr->type);
        return NULL;
    }

    return result.data;
}

/*
 * Find an equivalence class member expression, all of whose Vars, come from
 * the indicated relation.
 */
Expr *find_em_expr_for_rel(EquivalenceClass *ec, RelOptInfo *rel)
{
    ListCell   *lc_em;

    foreach(lc_em, ec->ec_members)
    {
        EquivalenceMember *em = lfirst(lc_em);

        if (bms_equal(em->em_relids, rel->relids))
        {
            /*
             * If there is more than one equivalence member whose Vars are
             * taken entirely from this relation, we'll be content to choose
             * any one of those.
             */
            return em->em_expr;
        }
    }

    /* We didn't find any suitable equivalence class expression */
    return NULL;
}
