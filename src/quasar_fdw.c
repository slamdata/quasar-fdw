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

#include "postgres.h"

#include "quasar_fdw.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "postmaster/fork_process.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"

#include <curl/curl.h>

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
 * Curl callback functions
 */
static size_t header_handler(void *buffer, size_t size, size_t nmemb, void *buf);
static size_t body_handler(void *buffer, size_t size, size_t nmemb, void *userp);

/*
 * Private functions
 */
void createQuery(RelOptInfo *foreignrel, struct QuasarTable *quasarTable, QuasarFdwPlanState *plan);
struct QuasarTable *getQuasarTable(Oid foreigntableid, QuasarOpt *opt);
void getUsedColumns(Expr *expr, struct QuasarTable *quasarTable);


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

      QuasarFdwPlanState *fdwState;

      elog(DEBUG1, "entering function %s", __func__);

      baserel->rows = 0;

      fdwState = palloc0(sizeof(QuasarFdwPlanState));
      baserel->fdw_private = (void *) fdwState;

      /* initialize required state in fdw_private */

      QuasarOpt *opt = quasar_get_options(foreigntableid);
      struct QuasarTable *qTable = getQuasarTable(foreigntableid, opt);
      createQuery(baserel, qTable, fdwState);
      elog(DEBUG1, "quasar_fdw: query is %s", fdwState->query);
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

      startup_cost = 100;
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

      elog(DEBUG1, "entering function %s", __func__);

      QuasarFdwPlanState *fdwPlan = (QuasarFdwPlanState*) baserel->fdw_private;
      List * fdw_private = list_make1(fdwPlan->query);

      scan_clauses = extract_actual_clauses(scan_clauses, false);


      /* Create the ForeignScan node */
#if(PG_VERSION_NUM < 90500)
      return make_foreignscan(tlist,
                              scan_clauses,
                              baserel->relid,
                              NIL,  /* no expressions to evaluate */
                              fdw_private);       /* no private state either */
#else
      return make_foreignscan(tlist,
                              scan_clauses,
                              baserel->relid,
                              NIL,  /* no expressions to evaluate */
                              fdw_private,  /* no private state either */
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

      CopyState   cstate;
      QuasarFdwExecState *festate;
      QuasarOpt *opt;
      StringInfoData buf;
      char       *url, *query, *prefix;
      pid_t       pid;
      quasar_ipc_context ctx;
      List *fdw_private = ((ForeignScan*)node->ss.ps.plan)->fdw_private;


      /*
       * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
       */
      if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
          return;

      festate = (QuasarFdwExecState *) palloc(sizeof(QuasarFdwExecState));

      festate->copy_options =
          list_make2(
              makeDefElem("format", (Node*) makeString("csv")),
              makeDefElem("header", (Node*) makeInteger(1))
          );

      /* Fetch options of foreign table */
      opt = quasar_get_options(RelationGetRelid(node->ss.ss_currentRelation));

      initStringInfo(&buf);
      appendStringInfo(&buf, "%s/query/fs%s?q=",
                       opt->server, opt->path);
      url = buf.data;

      query = (char *) lfirst(list_head(fdw_private));

      prefix = create_tempprefix();
      snprintf(ctx.flagfn, sizeof(ctx.flagfn), "%s.flag", prefix);
      snprintf(ctx.datafn, sizeof(ctx.datafn), "%s.data", prefix);
      pfree(prefix);

      if (mkfifo(ctx.flagfn, S_IRUSR | S_IWUSR) != 0)
          elog(ERROR, "mkfifo failed(%d):%s", errno, ctx.flagfn);
      if (mkfifo(ctx.datafn, S_IRUSR | S_IWUSR) != 0)
          elog(ERROR, "mkfifo failed(%d):%s", errno, ctx.datafn);



      /*
       * Fork to maximize parallelism of input from HTTP and output to SQL.
       * The spawned child process cheats by on_exit_rest() to die immediately.
       */
      pid = fork_process();
      if (pid == 0)           /* child */
      {
          struct curl_slist *headers;
          CURL       *curl;
          int         sc;
          StringInfoData urlbuf;
          char *query_encoded;

          MyProcPid = getpid();   /* reset MyProcPid */

          curl = curl_easy_init();

          /*
           * The exit callback routines clean up
           * unnecessary resources holded the parent process.
           * The child dies silently when finishing its job.
           */
          on_exit_reset();

          /*
           * Set up request header list
           */
          headers = NULL;
          headers = curl_slist_append(headers, "Accept: text/csv");

          /*
           * Encode query
           */
          query_encoded = curl_easy_escape(curl, query, 0);
          initStringInfo(&urlbuf);
          appendStringInfo(&urlbuf, "%s%s", url, query_encoded);

          /*
           * Set up CURL instance.
           */
          curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
          curl_easy_setopt(curl, CURLOPT_URL, urlbuf.data);
          curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
          curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_handler);
          curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
          curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_handler);
          curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

          elog(DEBUG1, "curling %s", urlbuf.data);
          sc = curl_easy_perform(curl);
          if (ctx.datafp)
              FreeFile(ctx.datafp);
          if (sc != 0)
          {
              elog(NOTICE, "%s:curl_easy_perform = %d", url, sc);
              unlink(ctx.datafn);
          }
          curl_slist_free_all(headers);
          curl_easy_cleanup(curl);
          curl_free(query_encoded);

          proc_exit(0);
      }
      elog(DEBUG1, "child pid = %d", pid);

      {
          int     status;
          FILE   *fp;

          fp = AllocateFile(ctx.flagfn, PG_BINARY_R);
          read(fileno(fp), &status, sizeof(int));
          FreeFile(fp);
          unlink(ctx.flagfn);
          if (status != 200)
          {
              elog(ERROR, "bad input from API. Status code: %d", status);
          }
      }

      /*
       * Create CopyState from FDW options.  We always acquire all columns, so
       * as to match the expected ScanTupleSlot signature.
       */
      cstate = BeginCopyFrom(node->ss.ss_currentRelation,
                             ctx.datafn,
                             false,
                             NIL,
                             festate->copy_options);

      /*
       * Save state in node->fdw_state.  We must save enough information to call
       * BeginCopyFrom() again.
       */
      festate->cstate = cstate;
      festate->datafn = pstrdup(ctx.datafn);

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

    elog(DEBUG1, "entering function %s", __func__);

    QuasarFdwExecState *festate = (QuasarFdwExecState *) node->fdw_state;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
    bool found;
    ErrorContextCallback errctx;

    /* Set up callback to identify error line number. */
    errctx.callback = CopyFromErrorCallback;
    errctx.arg = (void *) festate->cstate;
    errctx.previous = error_context_stack;
    error_context_stack = &errctx;

    /*
     * The protocol for loading a virtual tuple into a slot is first
     * ExecClearTuple, then fill the values/isnull arrays, then
     * ExecStoreVirtualTuple.  If we don't find another row in the file, we
     * just skip the last step, leaving the slot empty as required.
     *
     * We can pass ExprContext = NULL because we read all columns from the
     * file, so no need to evaluate default expressions.
     *
     * We can also pass tupleOid = NULL because we don't allow oids for
     * foreign tables.
     */
    ExecClearTuple(slot);
    found = NextCopyFrom(festate->cstate, NULL,
                         slot->tts_values, slot->tts_isnull,
                         NULL);
    if (found)
        ExecStoreVirtualTuple(slot);

    /* Remove error callback. */
    error_context_stack = errctx.previous;

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
          EndCopyFrom(festate->cstate);
          unlink(festate->datafn);
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

    EndCopyFrom(festate->cstate);

    festate->cstate = BeginCopyFrom(node->ss.ss_currentRelation,
        festate->datafn,
        false,
        NIL,
        festate->copy_options);

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
    quasar_ipc_context *ctx = (quasar_ipc_context *) userp;

    if (strncmp(buffer, HTTP_1_1, strlen(HTTP_1_1)) == 0)
    {
        int     status;

        status = atoi((char *) buffer + strlen(HTTP_1_1) + 1);
        elog(DEBUG1, "curl response status %d", status);
        ctx->flagfp = AllocateFile(ctx->flagfn, PG_BINARY_W);
        write(fileno(ctx->flagfp), &status, sizeof(int));
        FreeFile(ctx->flagfp);
        if (status != 200)
        {
            ctx->datafp = NULL;
            /* interrupt */
            return 0;
        }
        /* iif success */
        ctx->datafp = AllocateFile(ctx->datafn, PG_BINARY_W);
    }

    return segsize;
}

static size_t
body_handler(void *buffer, size_t size, size_t nmemb, void *userp)
{
    elog(DEBUG1, "entering function %s", __func__);
    size_t      segsize = size * nmemb;
    quasar_ipc_context *ctx = (quasar_ipc_context *) userp;

    fwrite(buffer, size, nmemb, ctx->datafp);
    elog(DEBUG1, "wrote %ld bytes to curl buffer", segsize);

    return segsize;
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
void createQuery(RelOptInfo *foreignrel, struct QuasarTable *quasarTable, QuasarFdwPlanState *plan)
{
    ListCell *cell;
    bool first_col = true;
    int i;
    StringInfoData query;
    List *columnlist = foreignrel->reltargetlist,
        *conditions = foreignrel->baserestrictinfo;

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
        if (quasarTable->cols[i]->used)
        {
            if (first_col)
            {
                first_col = false;
                appendStringInfo(&query, "%s", quasarTable->cols[i]->name);
            }
            else
            {
                appendStringInfo(&query, ", %s", quasarTable->cols[i]->name);
            }
        }
    }
    /* dummy column if there is no result column we need from Quasar */
    if (first_col)
        appendStringInfo(&query, "'1'");
    appendStringInfo(&query, " FROM %s", quasarTable->name);

    plan->query = query.data;
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
            getUsedColumns(((RestrictInfo *)expr)->clause, quasarTable);
            break;
        case T_TargetEntry:
            getUsedColumns(((TargetEntry *)expr)->expr, quasarTable);
            break;
        case T_Const:
        case T_Param:
        case T_CaseTestExpr:
        case T_CoerceToDomainValue:
        case T_CurrentOfExpr:
            break;
        case T_Var:
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
            foreach(cell, ((WindowFunc *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_ArrayRef:
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
            foreach(cell, ((FuncExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_OpExpr:
            foreach(cell, ((OpExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_DistinctExpr:
            foreach(cell, ((DistinctExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_NullIfExpr:
            foreach(cell, ((NullIfExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_ScalarArrayOpExpr:
            foreach(cell, ((ScalarArrayOpExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_BoolExpr:
            foreach(cell, ((BoolExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_SubPlan:
            foreach(cell, ((SubPlan *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_AlternativeSubPlan:
            /* examine only first alternative */
            getUsedColumns((Expr *)linitial(((AlternativeSubPlan *)expr)->subplans), quasarTable);
            break;
        case T_NamedArgExpr:
            getUsedColumns(((NamedArgExpr *)expr)->arg, quasarTable);
            break;
        case T_FieldSelect:
            getUsedColumns(((FieldSelect *)expr)->arg, quasarTable);
            break;
        case T_RelabelType:
            getUsedColumns(((RelabelType *)expr)->arg, quasarTable);
            break;
        case T_CoerceViaIO:
            getUsedColumns(((CoerceViaIO *)expr)->arg, quasarTable);
            break;
        case T_ArrayCoerceExpr:
            getUsedColumns(((ArrayCoerceExpr *)expr)->arg, quasarTable);
            break;
        case T_ConvertRowtypeExpr:
            getUsedColumns(((ConvertRowtypeExpr *)expr)->arg, quasarTable);
            break;
        case T_CollateExpr:
            getUsedColumns(((CollateExpr *)expr)->arg, quasarTable);
            break;
        case T_CaseExpr:
            foreach(cell, ((CaseExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            getUsedColumns(((CaseExpr *)expr)->arg, quasarTable);
            getUsedColumns(((CaseExpr *)expr)->defresult, quasarTable);
            break;
        case T_CaseWhen:
            getUsedColumns(((CaseWhen *)expr)->expr, quasarTable);
            getUsedColumns(((CaseWhen *)expr)->result, quasarTable);
            break;
        case T_ArrayExpr:
            foreach(cell, ((ArrayExpr *)expr)->elements)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_RowExpr:
            foreach(cell, ((RowExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_RowCompareExpr:
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
            foreach(cell, ((CoalesceExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_MinMaxExpr:
            foreach(cell, ((MinMaxExpr *)expr)->args)
            {
                getUsedColumns((Expr *)lfirst(cell), quasarTable);
            }
            break;
        case T_XmlExpr:
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
            getUsedColumns(((NullTest *)expr)->arg, quasarTable);
            break;
        case T_BooleanTest:
            getUsedColumns(((BooleanTest *)expr)->arg, quasarTable);
            break;
        case T_CoerceToDomain:
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
