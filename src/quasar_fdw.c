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
#include "optimizer/paths.h"
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
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * We store various information in ForeignScan.fdw_private to pass it from
 * planner to executor.  Currently we store:
 *
 * 1) SELECT statement text to be sent to the remote server
 * 2) Integer list of attribute numbers retrieved by the SELECT
 *
 * These items are indexed with the enum FdwScanPrivateIndex, so an item
 * can be fetched with list_nth().  For example, to get the SELECT statement:
 *              sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
 */
enum FdwScanPrivateIndex
{
    /* SQL statement to execute remotely (as a String node) */
    FdwScanPrivateSelectSql,
    /* Integer list of attribute numbers retrieved by the SELECT */
    FdwScanPrivateRetrievedAttrs
};

/*
 * Execution state of a foreign scan using quasar_fdw
 */
typedef struct QuasarFdwScanState
{
    /* extracted fdw_private data */
    char           *query;                      /* text of SELECT command */
    List           *retrieved_attrs;    /* list of retrieved attribute numbers */

    int             numParams;              /* number of parameters passed to query */
    FmgrInfo       *param_flinfo;   /* output conversion functions for them */
    List           *param_exprs;        /* executable expressions for param values */
    const char    **param_values;  /* textual values of query parameters */

    QuasarConn *conn;
} QuasarFdwScanState;

/*
 * Workspace for analyzing a foreign table.
 */
typedef struct PgFdwAnalyzeState
{
    Relation       rel;         /* relcache entry for the foreign table */
    AttInMetadata *attinmeta;   /* attribute datatype conversion metadata */
    List          *retrieved_attrs;    /* attr numbers retrieved by query */

    /* collected sample rows */
    HeapTuple  *rows;                   /* array of size targrows */
    int         targrows;               /* target # of sample rows */
    int         numrows;                /* # of sample rows collected */

    /* for random sampling */
    double      samplerows;             /* # of rows fetched */
    double      rowstoskip;             /* # of rows to skip before next sample */

    /* working memory contexts */
    MemoryContext anl_cxt;              /* context for per-analyze lifespan data */
    MemoryContext temp_cxt;             /* context for per-tuple temporary data */
} PgFdwAnalyzeState;

/*
 * Identify the attribute where data conversion fails.
 */
typedef struct ConversionLocation
{
    Relation    rel;                    /* foreign table's relcache entry */
    AttrNumber  cur_attno;              /* attribute number being processed, or 0 */
} ConversionLocation;

/* Callback argument for ec_member_matches_foreign */
typedef struct
{
    Expr           *current;            /* current expr, or NULL if not yet found */
    List           *already_used;       /* expressions already dealt with */
} ec_member_foreign_arg;

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
 * Private functions
 */
static void estimate_path_cost_size(PlannerInfo *root,
                                    RelOptInfo *baserel,
                                    List *join_conds,
                                    List *pathkeys,
                                    double *p_rows,
                                    int *p_width,
                                    Cost *p_startup_cost,
                                    Cost *p_total_cost);
static bool ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel,
                                      EquivalenceClass *ec, EquivalenceMember *em,
                                      void *arg);
static void renderParams(QuasarFdwScanState *fsstate,
                         ExprContext *econtext);


/*
 * _PG_init
 *      Library load-time initalization.
 *      Sets exitHook() callback for backend shutdown.
 *      Also finds the OIDs of PostGIS the PostGIS geometry type.
 */
void
_PG_init(void)
{
    QuasarGlobalConnectionInit();
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

    QuasarFdwRelationInfo *fpinfo;
    ListCell   *lc;


    elog(DEBUG1, "entering function %s", __func__);

    /*
     * We use PgFdwRelationInfo to pass various information to subsequent
     * functions.
     */
    fpinfo = (QuasarFdwRelationInfo *) palloc0(sizeof(QuasarFdwRelationInfo));
    baserel->fdw_private = (void *) fpinfo;

    /* Look up foreign-table catalog info. */
    fpinfo->table = GetForeignTable(foreigntableid);
    fpinfo->server = GetForeignServer(fpinfo->table->serverid);

    /*
     * Extract user-settable option values.  Note that per-table setting of
     * use_remote_estimate overrides per-server setting.
     */
    fpinfo->use_remote_estimate = false;
    fpinfo->fdw_startup_cost = DEFAULT_FDW_STARTUP_COST;
    fpinfo->fdw_tuple_cost = DEFAULT_FDW_TUPLE_COST;
    fpinfo->shippable_extensions = NIL;

    foreach(lc, fpinfo->server->options)
    {
        DefElem    *def = (DefElem *) lfirst(lc);

        if (strcmp(def->defname, "use_remote_estimate") == 0)
            fpinfo->use_remote_estimate = defGetBoolean(def);
        else if (strcmp(def->defname, "fdw_startup_cost") == 0)
            fpinfo->fdw_startup_cost = strtod(defGetString(def), NULL);
        else if (strcmp(def->defname, "fdw_tuple_cost") == 0)
            fpinfo->fdw_tuple_cost = strtod(defGetString(def), NULL);
    }
    foreach(lc, fpinfo->table->options)
    {
        DefElem    *def = (DefElem *) lfirst(lc);

        if (strcmp(def->defname, "use_remote_estimate") == 0)
        {
            fpinfo->use_remote_estimate = defGetBoolean(def);
            break;                            /* only need the one value */
        }
    }

    /*
     * Identify which baserestrictinfo clauses can be sent to the remote
     * server and which can't.
     */
    classifyConditions(root, baserel, baserel->baserestrictinfo,
                       &fpinfo->remote_conds, &fpinfo->local_conds);

    /*
     * Identify which attributes will need to be retrieved from the remote
     * server.  These include all attrs needed for joins or final output, plus
     * all attrs used in the local_conds.  (Note: if we end up using a
     * parameterized scan, it's possible that some of the join clauses will be
     * sent to the remote and thus we wouldn't really need to retrieve the
     * columns used in them.  Doesn't seem worth detecting that case though.)
     */
    fpinfo->attrs_used = NULL;
    pull_varattnos((Node *) baserel->reltargetlist, baserel->relid,
                   &fpinfo->attrs_used);
    foreach(lc, fpinfo->local_conds)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

        pull_varattnos((Node *) rinfo->clause, baserel->relid,
                       &fpinfo->attrs_used);
    }

    /*
     * Compute the selectivity and cost of the local_conds, so we don't have
     * to do it over again for each path.  The best we can do for these
     * conditions is to estimate selectivity on the basis of local statistics.
     */
    fpinfo->local_conds_sel = clauselist_selectivity(root,
                                                     fpinfo->local_conds,
                                                     baserel->relid,
                                                     JOIN_INNER,
                                                     NULL);

    cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

    /*
     * If the table or the server is configured to use remote estimates,
     * connect to the foreign server and execute EXPLAIN to estimate the
     * number of rows selected by the restriction clauses, as well as the
     * average row width.  Otherwise, estimate using whatever statistics we
     * have locally, in a way similar to ordinary tables.
     */
    if (fpinfo->use_remote_estimate)
    {
        /*
         * Get cost/size estimates with help of remote server.  Save the
         * values in fpinfo so we don't need to do it again to generate the
         * basic foreign path.
         */
        estimate_path_cost_size(root, baserel, NIL, NIL,
                                       &fpinfo->rows, &fpinfo->width,
                                       &fpinfo->startup_cost,
                                       &fpinfo->total_cost);

        /* Report estimated baserel size to planner. */
        baserel->rows = fpinfo->rows;
        baserel->width = fpinfo->width;
    }
    else
    {
        /*
         * If the foreign table has never been ANALYZEd, it will have relpages
         * and reltuples equal to zero, which most likely has nothing to do
         * with reality.  We can't do a whole lot about that if we're not
         * allowed to consult the remote server, but we can use a hack similar
         * to plancat.c's treatment of empty relations: use a minimum size
         * estimate of 10 pages, and divide by the column-datatype-based width
         * estimate to get the corresponding number of tuples.
         */
        if (baserel->pages == 0 && baserel->tuples == 0)
        {
            baserel->pages = 10;
#if (PG_VERSION_NUM >= 90500)
            baserel->tuples =
                (10 * BLCKSZ) / (baserel->width +
                                 MAXALIGN(SizeofHeapTupleHeader));
#else
            baserel->tuples =
                (10 * BLCKSZ) / (baserel->width +
                                 sizeof(HeapTupleHeaderData));
#endif

        }

        /* Estimate baserel size as best we can with local statistics. */
        set_baserel_size_estimates(root, baserel);

        /* Fill in basically-bogus cost estimates for use later. */
        estimate_path_cost_size(root, baserel, NIL, NIL,
                                       &fpinfo->rows, &fpinfo->width,
                                       &fpinfo->startup_cost,
                                       &fpinfo->total_cost);
      }
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

    QuasarFdwRelationInfo *fpinfo = (QuasarFdwRelationInfo *) baserel->fdw_private;
    ForeignPath *path;
    List        *ppi_list;
    ListCell    *lc;
    List        *usable_pathkeys = NIL;

    elog(DEBUG1, "entering function %s", __func__);

    /*
     * Create simplest ForeignScan path node and add it to baserel.  This path
     * corresponds to SeqScan path of regular tables (though depending on what
     * baserestrict conditions we were able to send to remote, there might
     * actually be an indexscan happening there).  We already did all the work
     * to estimate cost and size of this path.
     */
    path = create_foreignscan_path(root, baserel,
                                   fpinfo->rows,
                                   fpinfo->startup_cost,
                                   fpinfo->total_cost,
                                   NIL, /* no pathkeys */
                                   NULL,                /* no outer rel either */
                                   NIL);                /* no fdw_private list */
    add_path(baserel, (Path *) path);

    /*
     * Determine whether we can potentially push query pathkeys to the remote
     * side, avoiding a local sort.
     */
    foreach(lc, root->query_pathkeys)
    {
        PathKey    *pathkey = (PathKey *) lfirst(lc);
        EquivalenceClass *pathkey_ec = pathkey->pk_eclass;
        Expr       *em_expr;

        /*
         * is_foreign_expr would detect volatile expressions as well, but
         * ec_has_volatile saves some cycles.
         */
        if (!pathkey_ec->ec_has_volatile &&
            /* Quasar doesn't support NULLS FIRST
             * but DESC makes pk_nulls_first true regardless
             */
            (pathkey->pk_strategy != BTLessStrategyNumber ||
             !pathkey->pk_nulls_first) &&
            (em_expr = find_em_expr_for_rel(pathkey_ec, baserel)) &&
            is_foreign_expr(root, baserel, em_expr))
            usable_pathkeys = lappend(usable_pathkeys, pathkey);
        else
        {
            /*
             * The planner and executor don't have any clever strategy for
             * taking data sorted by a prefix of the query's pathkeys and
             * getting it to be sorted by all of those pathekeys.  We'll just
             * end up resorting the entire data set.  So, unless we can push
             * down all of the query pathkeys, forget it.
             */
            list_free(usable_pathkeys);
            usable_pathkeys = NIL;
            break;
        }
    }

    /* Create a path with useful pathkeys, if we found one. */
    if (usable_pathkeys != NULL)
    {
        double          rows;
        int             width;
        Cost            startup_cost;
        Cost            total_cost;

        estimate_path_cost_size(root, baserel, NIL, usable_pathkeys,
                                       &rows, &width,
                                       &startup_cost,
                                       &total_cost);

        add_path(baserel, (Path *)
                 create_foreignscan_path(root, baserel,
                                         rows,
                                         startup_cost,
                                         total_cost,
                                         usable_pathkeys,
                                         NULL,
                                         NIL));
    }

    /*
     * If we're not using remote estimates, stop here.  We have no way to
     * estimate whether any join clauses would be worth sending across, so
     * don't bother building parameterized paths.
     */
    if (!fpinfo->use_remote_estimate)
        return;

    /*
     * Thumb through all join clauses for the rel to identify which outer
     * relations could supply one or more safe-to-send-to-remote join clauses.
     * We'll build a parameterized path for each such outer relation.
     *
     * It's convenient to manage this by representing each candidate outer
     * relation by the ParamPathInfo node for it.  We can then use the
     * ppi_clauses list in the ParamPathInfo node directly as a list of the
     * interesting join clauses for that rel.  This takes care of the
     * possibility that there are multiple safe join clauses for such a rel,
     * and also ensures that we account for unsafe join clauses that we'll
     * still have to enforce locally (since the parameterized-path machinery
     * insists that we handle all movable clauses).
     */
    ppi_list = NIL;
    foreach(lc, baserel->joininfo)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
        Relids          required_outer;
        ParamPathInfo *param_info;

        /* Check if clause can be moved to this rel */
        if (!join_clause_is_movable_to(rinfo, baserel))
            continue;

        /* See if it is safe to send to remote */
        if (!is_foreign_expr(root, baserel, rinfo->clause))
            continue;

        /* Calculate required outer rels for the resulting path */
        required_outer = bms_union(rinfo->clause_relids,
                                   baserel->lateral_relids);
        /* We do not want the foreign rel itself listed in required_outer */
        required_outer = bms_del_member(required_outer, baserel->relid);

        /*
         * required_outer probably can't be empty here, but if it were, we
         * couldn't make a parameterized path.
         */
        if (bms_is_empty(required_outer))
            continue;

        /* Get the ParamPathInfo */
        param_info = get_baserel_parampathinfo(root, baserel,
                                               required_outer);
        Assert(param_info != NULL);

        /*
         * Add it to list unless we already have it.  Testing pointer equality
         * is OK since get_baserel_parampathinfo won't make duplicates.
         */
        ppi_list = list_append_unique_ptr(ppi_list, param_info);
    }

    /*
     * The above scan examined only "generic" join clauses, not those that
     * were absorbed into EquivalenceClauses.  See if we can make anything out
     * of EquivalenceClauses.
     */
    if (baserel->has_eclass_joins)
    {
        /*
         * We repeatedly scan the eclass list looking for column references
         * (or expressions) belonging to the foreign rel.  Each time we find
         * one, we generate a list of equivalence joinclauses for it, and then
         * see if any are safe to send to the remote.  Repeat till there are
         * no more candidate EC members.
         */
        ec_member_foreign_arg arg;

        arg.already_used = NIL;
        for (;;)
        {
            List           *clauses;

            /* Make clauses, skipping any that join to lateral_referencers */
            arg.current = NULL;
            clauses = generate_implied_equalities_for_column(root,
                                                             baserel,
                                                             ec_member_matches_foreign,
                                                             (void *) &arg,
                                                             baserel->lateral_referencers);

            /* Done if there are no more expressions in the foreign rel */
            if (arg.current == NULL)
            {
                Assert(clauses == NIL);
                break;
            }

            /* Scan the extracted join clauses */
            foreach(lc, clauses)
            {
                RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
                Relids          required_outer;
                ParamPathInfo *param_info;

                /* Check if clause can be moved to this rel */
                if (!join_clause_is_movable_to(rinfo, baserel))
                    continue;

                /* See if it is safe to send to remote */
                if (!is_foreign_expr(root, baserel, rinfo->clause))
                    continue;

                /* Calculate required outer rels for the resulting path */
                required_outer = bms_union(rinfo->clause_relids,
                                           baserel->lateral_relids);
                required_outer = bms_del_member(required_outer, baserel->relid);
                if (bms_is_empty(required_outer))
                    continue;

                /* Get the ParamPathInfo */
                param_info = get_baserel_parampathinfo(root, baserel,
                                                       required_outer);
                Assert(param_info != NULL);

                /* Add it to list unless we already have it */
                ppi_list = list_append_unique_ptr(ppi_list, param_info);
            }

            /* Try again, now ignoring the expression we found this time */
            arg.already_used = lappend(arg.already_used, arg.current);
        }
    }

    /*
     * Now build a path for each useful outer relation.
     */
    foreach(lc, ppi_list)
    {
        ParamPathInfo *param_info = (ParamPathInfo *) lfirst(lc);
        double         rows;
        int            width;
        Cost           startup_cost;
        Cost           total_cost;

        /* Get a cost estimate from the remote */
        estimate_path_cost_size(root, baserel,
                                       param_info->ppi_clauses, NIL,
                                       &rows, &width,
                                       &startup_cost,
                                       &total_cost);

        /*
         * ppi_rows currently won't get looked at by anything, but still we
         * may as well ensure that it matches our idea of the rowcount.
         */
        param_info->ppi_rows = rows;

        /* Make the path */
        path = create_foreignscan_path(root, baserel,
                                       rows,
                                       startup_cost,
                                       total_cost,
                                       NIL,             /* no pathkeys */
                                       param_info->ppi_req_outer,
                                       NIL);    /* no fdw_private list */
        add_path(baserel, (Path *) path);
    }
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

    QuasarFdwRelationInfo *fpinfo = (QuasarFdwRelationInfo *) baserel->fdw_private;
    Index           scan_relid = baserel->relid;
    List           *fdw_private;
    List           *remote_conds = NIL;
    List           *remote_exprs = NIL;
    List           *local_exprs = NIL;
    List           *params_list = NIL;
    List           *retrieved_attrs;
    StringInfoData  sql;
    ListCell       *lc;

    elog(DEBUG1, "entering function %s", __func__);

    /*
     * Separate the scan_clauses into those that can be executed remotely and
     * those that can't.  baserestrictinfo clauses that were previously
     * determined to be safe or unsafe by classifyConditions are shown in
     * fpinfo->remote_conds and fpinfo->local_conds.  Anything else in the
     * scan_clauses list will be a join clause, which we have to check for
     * remote-safety.
     *
     * Note: the join clauses we see here should be the exact same ones
     * previously examined by postgresGetForeignPaths.  Possibly it'd be worth
     * passing forward the classification work done then, rather than
     * repeating it here.
     *
     * This code must match "extract_actual_clauses(scan_clauses, false)"
     * except for the additional decision about remote versus local execution.
     * Note however that we don't strip the RestrictInfo nodes from the
     * remote_conds list, since appendWhereClause expects a list of
     * RestrictInfos.
     */
    foreach(lc, scan_clauses)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

        Assert(IsA(rinfo, RestrictInfo));

        /* Ignore any pseudoconstants, they're dealt with elsewhere */
        if (rinfo->pseudoconstant)
            continue;

        if (list_member_ptr(fpinfo->remote_conds, rinfo))
        {
            remote_conds = lappend(remote_conds, rinfo);
            remote_exprs = lappend(remote_exprs, rinfo->clause);
        }
        else if (list_member_ptr(fpinfo->local_conds, rinfo))
            local_exprs = lappend(local_exprs, rinfo->clause);
        else if (is_foreign_expr(root, baserel, rinfo->clause))
        {
            remote_conds = lappend(remote_conds, rinfo);
            remote_exprs = lappend(remote_exprs, rinfo->clause);
        }
        else
            local_exprs = lappend(local_exprs, rinfo->clause);
    }

    /*
     * Build the query string to be sent for execution, and identify
     * expressions to be sent as parameters.
     */
    initStringInfo(&sql);
    deparseSelectSql(&sql, root, baserel, fpinfo->attrs_used,
                     &retrieved_attrs, false);
    if (remote_conds)
        appendWhereClause(&sql, root, baserel, remote_conds,
                          true, &params_list);

    /* Add ORDER BY clause if we found any useful pathkeys */
    if (best_path->path.pathkeys)
        appendOrderByClause(&sql, root, baserel, best_path->path.pathkeys);

    /*
     * Build the fdw_private list that will be available to the executor.
     * Items in the list must match enum FdwScanPrivateIndex, above.
     */
    fdw_private = list_make2(makeString(sql.data),
                             retrieved_attrs);

    /*
     * Create the ForeignScan node from target list, local filtering
     * expressions, remote parameter expressions, and FDW private information.
     *
     * Note that the remote parameter expressions are stored in the fdw_exprs
     * field of the finished plan node; we can't keep them in private state
     * because then they wouldn't be subject to later planner processing.
     */
#if(PG_VERSION_NUM < 90500)
    elog(DEBUG1, "make_foreignscan %d", baserel->relid);
    return make_foreignscan(tlist,
                            local_exprs,
                            scan_relid,
                            params_list,
                            fdw_private);
#else
    return make_foreignscan(tlist,
                            local_exprs,
                            scan_relid,
                            params_list,
                            fdw_private,
                            NIL,        /* no custom tlist */
                            remote_exprs);
#endif  /* PG_VERSION_NUM */
}


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
    ForeignScan *fsplan = (ForeignScan *)node->ss.ps.plan;
    EState *estate = node->ss.ps.state;
    QuasarFdwScanState *fsstate;
    ForeignTable *table;
    ForeignServer *server;
    int numParams;
    int i;
    ListCell *lc;
    Relation rel;
    AttInMetadata *attinmeta;

    elog(DEBUG1, "entering function %s", __func__);

    /*
     * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
        return;

    /*
     * We'll save private state in node->fdw_state.
     */
    fsstate = (QuasarFdwScanState *) palloc0(sizeof(QuasarFdwScanState));
    node->fdw_state = (void *) fsstate;

    /* Get info about foreign table. */
    rel = node->ss.ss_currentRelation;
    table = GetForeignTable(RelationGetRelid(rel));
    server = GetForeignServer(table->serverid);

    /*
     * Get connection to the foreign server.
     */
    fsstate->conn = QuasarGetConnection(server);

    /* Get private info created by planner functions. */
    fsstate->query = strVal(list_nth(fsplan->fdw_private,
                                     FdwScanPrivateSelectSql));

    /* Get info we'll need for input data conversion. */
    attinmeta = TupleDescGetAttInMetadata(RelationGetDescr(rel));

    /* Prepare our connection for a query */
    QuasarPrepQuery(fsstate->conn, estate, attinmeta);

    /* prepare for output conversion of parameters used in remote query. */
    numParams = list_length(fsplan->fdw_exprs);
    fsstate->numParams = numParams;
    fsstate->param_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * numParams);

    i = 0;
    foreach(lc, fsplan->fdw_exprs)
    {
        Node       *param_expr = (Node *) lfirst(lc);
        Oid         typefnoid;
        bool        isvarlena;

        getTypeOutputInfo(exprType(param_expr), &typefnoid, &isvarlena);
        fmgr_info(typefnoid, &fsstate->param_flinfo[i]);
        i++;
    }

    /*
     * Prepare remote-parameter expressions for evaluation.  (Note: in
     * practice, we expect that all these expressions will be just Params, so
     * we could possibly do something more efficient than using the full
     * expression-eval machinery for this.  But probably there would be little
     * benefit, and it'd require postgres_fdw to know more than is desirable
     * about Param evaluation.)
     */
    fsstate->param_exprs = (List *)
        ExecInitExpr((Expr *) fsplan->fdw_exprs,
                     (PlanState *) node);

    /*
     * Allocate buffer for text form of query parameters, if any.
     */
    if (numParams > 0)
        fsstate->param_values = (const char **) palloc0(numParams * sizeof(char *));
    else
        fsstate->param_values = NULL;

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

    QuasarFdwScanState *fsstate = (QuasarFdwScanState *)node->fdw_state;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
    ExprContext *econtext = node->ss.ps.ps_ExprContext;

    elog(DEBUG4, "entering function %s", __func__);

    /*
     * If this is the first call after Begin or ReScan, we need to create the
     * cursor on the remote side.
     */
    if (fsstate->conn->exec_transfer == 0) {
        renderParams(fsstate, econtext);
        QuasarExecuteQuery(fsstate->conn, fsstate->query,
                           fsstate->param_values, fsstate->numParams);
    }

    /*
     * Get some more tuples, if we've run out.
     */
    if (fsstate->conn->qctx->next_tuple >= fsstate->conn->qctx->num_tuples)
    {
        QuasarContinueQuery(fsstate->conn);

        /* If we didn't get any tuples, must be end of data. */
        if (fsstate->conn->qctx->next_tuple >= fsstate->conn->qctx->num_tuples)
            return ExecClearTuple(slot);
    }


    /*
     * Return the next tuple.
     */
    ExecStoreTuple(fsstate->conn->qctx->tuples[fsstate->conn->qctx->next_tuple++],
                   slot,
                   InvalidBuffer,
                   false);

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

      QuasarFdwScanState *festate = (QuasarFdwScanState *) node->fdw_state;

      /* if festate is NULL, we are in EXPLAIN; nothing to do */
      if (festate) {
          QuasarCleanupConnection(festate->conn);
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
    QuasarFdwScanState *fsstate = (QuasarFdwScanState *) node->fdw_state;

    QuasarRewindQuery(fsstate->conn);
 }


/*
 * quasarExplainForeignScan
 *              Produce extra output for EXPLAIN:
 *              the Quasar query
 */
static void
quasarExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
    elog(DEBUG1, "Entering function %s", __func__);

    List           *fdw_private;
    char           *sql;
    char           *mongo_query;
    QuasarConn     *conn;
    ForeignTable   *table;
    ForeignServer  *server;

    fdw_private = ((ForeignScan *) node->ss.ps.plan)->fdw_private;
    sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));

    ExplainPropertyText("Quasar query", sql, es);
    if (es->verbose)
    {
        table = GetForeignTable(RelationGetRelid(node->ss.ss_currentRelation));
        server = GetForeignServer(table->serverid);
        conn = QuasarGetConnection(server);

        mongo_query = QuasarCompileQuery(conn, sql);
        QuasarCleanupConnection(conn);
        ExplainPropertyText("Compiled Mongo Query", mongo_query, es);
    }
}

/*
 * Detect whether we want to process an EquivalenceClass member.
 *
 * This is a callback for use by generate_implied_equalities_for_column.
 */
static bool
ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel,
                          EquivalenceClass *ec, EquivalenceMember *em,
                          void *arg)
{
    ec_member_foreign_arg *state = (ec_member_foreign_arg *) arg;
    Expr           *expr = em->em_expr;

    /*
     * If we've identified what we're processing in the current scan, we only
     * want to match that expression.
     */
    if (state->current != NULL)
        return equal(expr, state->current);

    /*
     * Otherwise, ignore anything we've already processed.
     */
    if (list_member(state->already_used, expr))
        return false;

    /* This is the new target to process. */
    state->current = expr;
    return true;
}


/*
 * Find an equivalence class member expression, all of whose Vars, come from
 * the indicated relation.
 */
extern Expr *
find_em_expr_for_rel(EquivalenceClass *ec, RelOptInfo *rel)
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


/*
 * estimate_path_cost_size
 *              Get cost and size estimates for a foreign scan
 *
 * We assume that all the baserestrictinfo clauses will be applied, plus
 * any join clauses listed in join_conds.
 */
static void
estimate_path_cost_size(PlannerInfo *root,
                        RelOptInfo *baserel,
                        List *join_conds,
                        List *pathkeys,
                        double *p_rows, int *p_width,
                        Cost *p_startup_cost, Cost *p_total_cost)
{
    QuasarFdwRelationInfo *fpinfo = (QuasarFdwRelationInfo *) baserel->fdw_private;
    double              rows;
    double              retrieved_rows;
    int                 width;
    Cost                startup_cost;
    Cost                total_cost;
    Cost                run_cost;
    Cost                cpu_per_tuple;

    /*
     * If the table or the server is configured to use remote estimates,
     * connect to the foreign server and execute a COUNT to estimate the
     * number of rows selected by the restriction+join clauses.  Otherwise,
     * estimate rows using whatever statistics we have locally, in a way
     * similar to ordinary tables.
     */
    if (fpinfo->use_remote_estimate)
    {
        List       *remote_join_conds;
        List       *local_join_conds;
        StringInfoData sql;
        List       *retrieved_attrs;
        Selectivity local_sel;
        QualCost        local_cost;
        QuasarConn *conn;

        /*
         * join_conds might contain both clauses that are safe to send across,
         * and clauses that aren't.
         */
        classifyConditions(root, baserel, join_conds,
                           &remote_join_conds, &local_join_conds);

        /*
         * Construct a SELECT count(*) with only the WHERE clauses.
         * Since quasar doesn't have an EXPLAIN API, we can't include things
         * that don't impact the row count like join or ordering
         **/
        initStringInfo(&sql);
        deparseSelectSql(&sql, root, baserel, fpinfo->attrs_used,
                         &retrieved_attrs, true);
        if (fpinfo->remote_conds)
            appendWhereClause(&sql, root, baserel, fpinfo->remote_conds,
                              true, NULL);
        /* if (remote_join_conds) */
        /*     appendWhereClause(&sql, root, baserel, remote_join_conds, */
        /*                       (fpinfo->remote_conds == NIL), NULL); */

        /* if (pathkeys) */
        /*     appendOrderByClause(&sql, root, baserel, pathkeys); */

        /* Get the remote estimate */
        conn = QuasarGetConnection(fpinfo->server);
        rows = QuasarEstimateRows(conn, sql.data);
        QuasarCleanupConnection(conn);

        /* Ideally quasar gives these to us but we have to improvise */
        width = baserel->width;
        startup_cost = QUASAR_STARTUP_COST;
        total_cost = startup_cost + rows * QUASAR_PER_TUPLE_COST;

        retrieved_rows = rows;

        /* Estimate less rows with a join condition */
        if (remote_join_conds)
            rows *= FDW_JOIN_MULTIPLIER;

        /* Factor in the selectivity of the locally-checked quals */
        local_sel = clauselist_selectivity(root,
                                           local_join_conds,
                                           baserel->relid,
                                           JOIN_INNER,
                                           NULL);
        local_sel *= fpinfo->local_conds_sel;

        rows = clamp_row_est(rows * local_sel);

        /* Add in the eval cost of the locally-checked quals */
        startup_cost += fpinfo->local_conds_cost.startup;
        total_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;
        cost_qual_eval(&local_cost, local_join_conds, root);
        startup_cost += local_cost.startup;
        total_cost += local_cost.per_tuple * retrieved_rows;
    } else {

        /* Use rows/width estimates made by set_baserel_size_estimates. */
        rows = baserel->rows;
        width = baserel->width;

        /*
         * Back into an estimate of the number of retrieved rows.  Just in
         * case this is nuts, clamp to at most baserel->tuples.
         */
        retrieved_rows = clamp_row_est(rows / fpinfo->local_conds_sel);
        retrieved_rows = Min(retrieved_rows, baserel->tuples);

        /*
         * Cost as though this were a seqscan, which is pessimistic.  We
         * effectively imagine the local_conds are being evaluated remotely,
         * too.
         */
        startup_cost = 0;
        run_cost = 0;
        run_cost += seq_page_cost * baserel->pages;

        startup_cost += baserel->baserestrictcost.startup;
        cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
        run_cost += cpu_per_tuple * baserel->tuples;

        /*
         * Without remote estimates, we have no real way to estimate the cost
         * of generating sorted output.  It could be free if the query plan
         * the remote side would have chosen generates properly-sorted output
         * anyway, but in most cases it will cost something.  Estimate a value
         * high enough that we won't pick the sorted path when the ordering
         * isn't locally useful, but low enough that we'll err on the side of
         * pushing down the ORDER BY clause when it's useful to do so.
         */
        if (pathkeys != NIL)
        {
            startup_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
            run_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
        }

        total_cost = startup_cost + run_cost;
    }

    /*
     * Add some additional cost factors to account for connection overhead
     * (fdw_startup_cost), transferring data across the network
     * (fdw_tuple_cost per retrieved row), and local manipulation of the data
     * (cpu_tuple_cost per retrieved row).
     */
    startup_cost += fpinfo->fdw_startup_cost;
    total_cost += fpinfo->fdw_startup_cost;
    total_cost += fpinfo->fdw_tuple_cost * retrieved_rows;
    total_cost += cpu_tuple_cost * retrieved_rows;

    /* Return results. */
    *p_rows = rows;
    *p_width = width;
    *p_startup_cost = startup_cost;
    *p_total_cost = total_cost;
}

/* Render parameters in foreign scan state */
static void
renderParams(QuasarFdwScanState *fsstate, ExprContext *econtext)
{
    /*
     * Construct array of query parameter values in text format.  We do the
     * conversions in the short-lived per-tuple context, so as not to cause a
     * memory leak over repeated scans.
     */
    if (fsstate->numParams > 0)
    {
        MemoryContext oldcontext;
        int           i;
        ListCell     *lc;

        oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

        i = 0;
        foreach(lc, fsstate->param_exprs)
        {
            ExprState  *expr_state = (ExprState *) lfirst(lc);
            Datum       expr_value;
            bool        isNull;

            /* Evaluate the parameter expression */
            expr_value = ExecEvalExpr(expr_state, econtext, &isNull, NULL);

            /*
             * Get string representation of each parameter value by invoking
             * type-specific output function, unless the value is null.
             */
            if (isNull)
                fsstate->param_values[i] = NULL;
            else
                fsstate->param_values[i] =
                    OutputFunctionCall(&fsstate->param_flinfo[i],
                                       expr_value);
            i++;
        }

        MemoryContextSwitchTo(oldcontext);
    }
}
