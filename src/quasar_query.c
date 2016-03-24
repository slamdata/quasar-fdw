/*-------------------------------------------------------------------------
 *
 * Quasar Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2015 SlamData
 *
 * This software is released under the Apache 2 license
 *
 * Author: Jon Eisen <jon@joneisen.works>
 *
 * IDENTIFICATION
 *            quasar_fdw/src/quasar_query.h
 *
 * Query deparser for quasar_fdw
 *
 * This file includes functions that examine query WHERE clauses to see
 * whether they're safe to send to the remote server for execution, as
 * well as functions to construct the query text to be sent.
 * One saving grace is that we only need deparse logic for node types that
 * we consider safe to send.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "quasar_fdw.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "datatype/timestamp.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "pgtime.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "utils/syscache.h"


/*
 * Global context for foreign_expr_walker's search of an expression tree.
 */
typedef struct foreign_glob_cxt
{
    PlannerInfo *root;                      /* global planner state */
    RelOptInfo *foreignrel;         /* the foreign relation we are planning for */
} foreign_glob_cxt;

/*
 * Context for deparseExpr
 */
typedef struct deparse_expr_cxt
{
    PlannerInfo *root;                      /* global planner state */
    RelOptInfo *foreignrel;         /* the foreign relation we are planning for */
    StringInfo      buf;                    /* output buffer to append to */
    List      **params_list;        /* exprs that will become remote Params */
} deparse_expr_cxt;

/*
 * Functions to determine whether an expression can be evaluated safely on
 * remote server.
 */
static bool foreign_expr_walker(Node *node,
                                foreign_glob_cxt *glob_cxt);

/*
 * Functions to construct string representation of a node tree.
 */
#if(PG_VERSION_NUM < 90500)
static void deparseTargetList(StringInfo buf,
                              PlannerInfo *root,
                              Index rtindex,
                              Relation rel,
                              Bitmapset *attrs_used);
#else /* PG_VERSION_NUM < 90500 */
static void deparsePushdownTargetList(StringInfo buf,
                                      PlannerInfo *root,
                                      Index rtindex,
                                      Relation rel,
                                      List *columnlist,
                                      Bitmapset *attrs_used,
                                      List **scan_tlist);
#endif /* PG_VERSION_NUM < 90500 */
static void deparseColumnRef(StringInfo buf, int varno, int varattno,
                             PlannerInfo *root, bool selector);
static void deparseRelation(StringInfo buf, Relation rel);
static void deparseExpr(Expr *expr, deparse_expr_cxt *context);
static void deparseVar(Var *node, deparse_expr_cxt *context);
static void deparseConst(Const *node, deparse_expr_cxt *context);
static void deparseParam(Param *node, deparse_expr_cxt *context);
static void deparseArrayRef(ArrayRef *node, deparse_expr_cxt *context);
static void deparseFuncExpr(FuncExpr *node, deparse_expr_cxt *context);
static void deparseOpExpr(OpExpr *node, deparse_expr_cxt *context);
static void deparseScalarArrayOpExpr(ScalarArrayOpExpr *node,
                                     deparse_expr_cxt *context);
static void deparseRelabelType(RelabelType *node, deparse_expr_cxt *context);
static void deparseBoolExpr(BoolExpr *node, deparse_expr_cxt *context);
static void deparseNullTest(NullTest *node, deparse_expr_cxt *context);
static void deparseArrayExpr(ArrayExpr *node, deparse_expr_cxt *context);
static void deparseArrayLiteral(StringInfo buf, Datum value, Oid type, Oid elemType, bool use_set_syntax);
static void printRemoteParam(int paramindex, Oid paramtype, int32 paramtypmod,
                             deparse_expr_cxt *context);
static void printRemotePlaceholder(Oid paramtype, int32 paramtypmod,
                                   deparse_expr_cxt *context);
static char * quasar_quote_identifier(const char *s);
static Oid getElementType(Oid type, int depth);

/* Functions used for rendering query and checking quasar ability */
bool quasar_can_handle_type(Oid type);
bool quasar_has_const(Const *node);
bool quasar_has_function(FuncExpr *func, char **name);
bool quasar_has_op(OpExpr *op, char **name, char *opkind, bool *asfunc);
bool quasar_has_scalar_array_op(ScalarArrayOpExpr *arrayoper, char **name);
bool quasar_can_pushdown_column(int varno, int varattno, PlannerInfo *root);

/*
 * Examine each qual clause in input_conds, and classify them into two groups,
 * which are returned as two lists:
 *      - remote_conds contains expressions that can be evaluated remotely
 *      - local_conds contains expressions that can't be evaluated remotely
 */
void
classifyConditions(PlannerInfo *root,
                   RelOptInfo *baserel,
                   List *input_conds,
                   List **remote_conds,
                   List **local_conds)
{
    ListCell   *lc;

    *remote_conds = NIL;
    *local_conds = NIL;

    foreach(lc, input_conds)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

        if (is_foreign_expr(root, baserel, ri->clause))
        {
            *remote_conds = lappend(*remote_conds, ri);
        }
        else
        {
            *local_conds = lappend(*local_conds, ri);
        }
    }
}

/*
 * Returns true if given expr is safe to evaluate on the foreign server.
 */
bool
is_foreign_expr(PlannerInfo *root,
                RelOptInfo *baserel,
                Expr *expr)
{
    foreign_glob_cxt glob_cxt;

    /*
     * Check that the expression consists of nodes that are safe to execute
     * remotely.
     */
    glob_cxt.root = root;
    glob_cxt.foreignrel = baserel;
    if (!foreign_expr_walker((Node *) expr, &glob_cxt))
        return false;


    /* OK to evaluate on the remote server */
    return true;
}

/*
 * These macros is used by foreign_expr_walker to identify PostgreSQL
 * types that can be translated to Quasar SQL-2.
 */
#define checkType(type) if (!quasar_can_handle_type((type))) return false
#define checkCollation(collid) if (collid != InvalidOid &&              \
                                   collid != DEFAULT_COLLATION_OID) return false
#define foreign_recurse(arg)\
    if (!foreign_expr_walker((Node *) (arg), glob_cxt)) \
        return false
#define IS_INTEGER(x) ((x) == INT2OID || (x) == INT4OID || (x) == INT8OID)

/*
 * Check if expression is safe to execute remotely, and return true if so.
 *
 * In addition, *outer_cxt is updated with collation information.
 *
 * We must check that the expression contains only node types we can deparse,
 * that all types/functions/operators are safe to send (they are "shippable"),
 * and that all collations used in the expression derive from Vars of the
 * foreign table.  Because of the latter, the logic is pretty close to
 * assign_collations_walker() in parse_collate.c, though we can assume here
 * that the given expression is valid.  Note function mutability is not
 * currently considered here.
 *
 * Also checks to ensure types and operations are available on Quasar
 */
static bool
foreign_expr_walker(Node *node,
                    foreign_glob_cxt *glob_cxt)
{
    /* Need do nothing for empty subexpressions */
    if (node == NULL)
        return true;

    switch (nodeTag(node))
    {
    case T_Var:
    {
        Var                *var = (Var *) node;

        /*
         * If the Var is from the foreign table, we consider its
         * collation (if any) safe to use.  If it is from another
         * table, we treat its collation the same way as we would a
         * Param's collation, ie it's not safe for it to have a
         * non-default collation.
         */
        if (var->varno == glob_cxt->foreignrel->relid &&
            var->varlevelsup == 0)
        {
            /* Var belongs to foreign table */

            /*
             * System columns should not be sent to
             * the remote
             */
            if (var->varattno < 0)
                return false;

            /* Check to make sure `nopushdown` is not enable for this column */
            if (!quasar_can_pushdown_column(var->varno,
                                            var->varattno,
                                            glob_cxt->root))
                return false;

            checkCollation(var->varcollid);
        }
        else
        {
            checkType(var->vartype);
            checkCollation(var->varcollid);
        }
    }
    break;
    case T_Const:
    {
        Const      *c = (Const *) node;

        checkType(c->consttype);
        checkCollation(c->constcollid);

        if (!quasar_has_const(c))
            return false;
    }
    break;
    case T_Param:
    {
        Param      *p = (Param *) node;

        checkType(p->paramtype);
        checkCollation(p->paramcollid);
    }
    break;
    case T_ArrayRef:
    {
        ArrayRef   *ar = (ArrayRef *) node;

        /* Check to make sure element type is handleable */
        checkType(ar->refelemtype);
        checkCollation(ar->refcollid);

        /* Assignment should not be in restrictions. */
        if (ar->refassgnexpr != NULL)
            return false;

        /* Quasar can only handle single element references, not slicing */
        if (ar->reflowerindexpr != NULL) {
            return false;
        }

        /*
         * Recurse to remaining subexpressions.  Since the array
         * subscripts must yield (noncollatable) integers, they won't
         * affect the inner_cxt state.
         */
        foreign_recurse(ar->refupperindexpr);
        foreign_recurse(ar->refexpr);
    }
    break;
    case T_FuncExpr:
    {
        FuncExpr   *fe = (FuncExpr *) node;

        checkType(fe->funcresulttype);
        checkCollation(fe->inputcollid);
        checkCollation(fe->funccollid);

        /*
         * We don't have to do anything for implicit casts
         * But we don't allow anything else not supported by quasar
         */
        if (fe->funcformat != COERCE_IMPLICIT_CAST &&
            !quasar_has_function(fe, NULL))
            return false;

        /*
         * Recurse to input subexpressions.
         */
        foreign_recurse(fe->args);
    }
    break;
    case T_OpExpr:
    {
        OpExpr     *oe = (OpExpr *) node;

        checkCollation(oe->inputcollid);
        checkCollation(oe->opcollid);

        /* Similarly, only operators quasar has can be sent. */
        if (!quasar_has_op(oe, NULL, NULL, NULL))
            return false;

        /*
         * Recurse to input subexpressions.
         */
        foreign_recurse(oe->args);
    }
    break;
    case T_ScalarArrayOpExpr:
    {
        ScalarArrayOpExpr *oe = (ScalarArrayOpExpr *) node;

        checkCollation(oe->inputcollid);

        if (!quasar_has_scalar_array_op(oe, NULL))
            return false;

        foreign_recurse(oe->args);
    }
    break;
    case T_RelabelType:
    {
        RelabelType *r = (RelabelType *) node;

        /* Quasar doesn't really have types in the SQL sense
         * So there's nothing to do for a relabel of a binary-compat type
         */

        checkCollation(r->resultcollid);
        foreign_recurse(r->arg);
    }
    break;
    case T_BoolExpr:
    {
        BoolExpr   *b = (BoolExpr *) node;

        foreign_recurse(b->args);
    }
    break;
    case T_NullTest:
    {
        NullTest   *nt = (NullTest *) node;

        foreign_recurse(nt->arg);
    }
    break;
    case T_ArrayExpr:
    {
        ArrayExpr  *a = (ArrayExpr *) node;

        checkType(a->element_typeid);
        checkCollation(a->array_collid);

        foreign_recurse(a->elements);
    }
    break;
    case T_List:
    {
        List       *l = (List *) node;
        ListCell   *lc;

        /*
         * Recurse to component subexpressions.
         */
        foreach(lc, l)
        {
            foreign_recurse(lfirst(lc));
        }
    }
    break;
    default:

        /*
         * If it's anything else, assume it's unsafe.  This list can be
         * expanded later, but don't forget to add deparse support below.
         */
        return false;
    }

    /* It looks OK */
    return true;
}

/* quasar_has_function
 * Test to see if quasar has a function
 * If name is not NULL, put the function name into it
 * Quasar name may be different from PG name
 */
bool quasar_has_function(FuncExpr *func, char **name) {
    char *opername;
    Oid schema;

    /* get function name and schema */
    HeapTuple tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(func->funcid));
    if (! HeapTupleIsValid(tuple))
    {
        elog(ERROR, "cache lookup failed for function %u", func->funcid);
    }
    opername = pstrdup(((Form_pg_proc)GETSTRUCT(tuple))->proname.data);
    schema = ((Form_pg_proc)GETSTRUCT(tuple))->pronamespace;
    ReleaseSysCache(tuple);

    /* ignore functions in other than the pg_catalog schema */
    if (schema != PG_CATALOG_NAMESPACE)
        return false;

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
        if (name != NULL) {
            /* Render the quasar function name into *name */
            /* FUNCTION TRANSFORMS */
            if (strcmp(opername, "char_length") == 0 ||
                strcmp(opername, "character_length") == 0)
                *name = pstrdup("length");
            else if (strcmp(opername, "substr") == 0)
                *name = pstrdup("substring");
            else
                *name = pstrdup(opername);
        }

        pfree(opername);
        return true;
    }

    pfree(opername);
    return false;
}

/* Recursively get the element type of array types
 * Return argument if not an array type
 **/
Oid getElementType(Oid type, int depth)
{
    Oid elemType = 0;
    HeapTuple tp;

    tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
    if (HeapTupleIsValid(tp))
    {
        Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
        elemType = typtup->typelem;
        ReleaseSysCache(tp);
    }

    if (elemType != 0) {
        if (depth == 1)
            return elemType;
        else
            return getElementType(elemType, depth - 1);

    }
    return type;
}

/* Check to see if quasar can handle the type */
bool quasar_can_handle_type(Oid type)
{
    type = getElementType(type, -1);
    return (type == TEXTOID || type == CHAROID
            || type == BPCHAROID
            || type == VARCHAROID || type == NAMEOID
            || type == INT8OID || type == INT2OID
            || type == INT4OID || type == OIDOID
            || type == FLOAT4OID || type == FLOAT8OID
            || type == NUMERICOID || type == DATEOID
            || type == TIMEOID || type == TIMESTAMPOID
            || type == INTERVALOID || type == BOOLOID
            || type == TIMESTAMPTZOID);
}

/* quasar_has_const
 * Returns false if const is numeric type and NaN
 * True otherwise
 */
bool quasar_has_const(Const *node) {
    switch (node->consttype) {
    case INT2OID:
    case INT4OID:
    case INT8OID:
    case OIDOID:
    case FLOAT4OID:
    case FLOAT8OID:
    case NUMERICOID:
    {
        Oid typoutput;
        bool typisvarlena;
        char *value;

        getTypeOutputInfo(node->consttype, &typoutput, &typisvarlena);
        value = OidOutputFunctionCall(typoutput, node->constvalue);

        if (strcmp(value, "NaN") == 0)
            return false;
    }
    break;
    case INTERVALOID:
    {
        Interval *span = DatumGetIntervalP(node->constvalue);
        if (span->month > 0)
            return false;
    }
    break;
    }
    return true;
}

/* quasar_has_op
 * Test to see if quasar has a operator
 * If name is not NULL, put the operator name into it
 * If opkind is not NULL, put the opkind in it
 * Quasar name may be different from PG name
 */
bool quasar_has_op(OpExpr *oper, char **name, char *opkind, bool *asfunc) {
    Oid schema, rightargtype;
    char *opername, oprkind;

    /* get operator name, kind, argument type and schema */
    HeapTuple tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(oper->opno));
    if (! HeapTupleIsValid(tuple))
    {
        elog(ERROR, "cache lookup failed for operator %u", oper->opno);
    }
    opername = pstrdup(((Form_pg_operator)GETSTRUCT(tuple))->oprname.data);
    oprkind = ((Form_pg_operator)GETSTRUCT(tuple))->oprkind;
    schema = ((Form_pg_operator)GETSTRUCT(tuple))->oprnamespace;
    rightargtype = ((Form_pg_operator)GETSTRUCT(tuple))->oprright;
    ReleaseSysCache(tuple);

    /* ignore operators in other than the pg_catalog schema */
    if (schema != PG_CATALOG_NAMESPACE)
        return false;

    if (strcmp(opername, "=") == 0     /* equal to */
        || strcmp(opername, "<>") == 0 /* not equal to */
        || strcmp(opername, ">") == 0  /* greater than */
        || strcmp(opername, "<") == 0  /* less than */
        || strcmp(opername, ">=") == 0 /* greater than or equal to */
        || strcmp(opername, "<=") == 0 /* less than or equal to */
        || strcmp(opername, "+") == 0 /* positive, addition */
        || strcmp(opername, "/") == 0 /* division */
        /* Cannot subtract DATEs in Quasar */
        || (strcmp(opername, "-") == 0 /* negation, subtraction */
            && rightargtype != DATEOID
            && rightargtype != TIMESTAMPOID
            && rightargtype != TIMESTAMPTZOID)
        || strcmp(opername, "*") == 0 /* multiplication */
        /* Regexes in Quasar only take constant right sides
         * Also, Quasar doesn't have case insenstive LIKE operators (~~*)
         * But it does have the posix-regex-like operators*/
        || (strcmp(opername, "~~") == 0 /* case-senstive sql-regex */
            && ((Expr*)lsecond(oper->args))->type == T_Const)
        || (strcmp(opername, "!~~") == 0 /* case-sensitive NOT sql-regex */
            && ((Expr*)lsecond(oper->args))->type == T_Const)
        || strcmp(opername, "~") == 0 /* case-sensitive posix-regex */
        || strcmp(opername, "!~") == 0 /* case-sensitive NOT posix-regex */
        || strcmp(opername, "~*") == 0 /* case-insensitive posix-regex */
        || strcmp(opername, "!~*") == 0 /* case-insensititve NOT posix-regex */
        || strcmp(opername, "%") == 0 /* modulus */
        || strcmp(opername, "||") == 0 /* Text Concatenation */
        )
    {
        if (name != NULL && asfunc != NULL) {
            *asfunc = false;

            if (oprkind == 'b') /* BINARY OPERATIONS */
            {
                if (strcmp(opername, "~~") == 0)
                    *name = pstrdup("LIKE");
                else if (strcmp(opername, "!~~") == 0)
                    *name = pstrdup("NOT LIKE");
                else if (strcmp(opername, "||") == 0)
                {
                    *name = pstrdup("concat");
                    *asfunc = true;
                }
                else
                    *name = pstrdup(opername);
            }
            else                /* UNARY OPERATIONS */
            {
                *name = pstrdup(opername);
            }
        }

        if (opkind != NULL) *opkind = oprkind;

        pfree(opername);
        return true;
    }

    pfree(opername);
    return false;
}

/* quasar_has_scalar_array_op
 * Test to see if quasar has a scalar array operator
 * such as (ANY, ALL, IN, NOT IN)
 * If name is not NULL, put the operator name into it
 * Quasar name may be different from PG name
 */
bool quasar_has_scalar_array_op(ScalarArrayOpExpr *arrayoper, char **name) {
    char *opername;
    Oid schema;
    HeapTuple tuple;
    Const *right;
    ArrayType *a;

    /* get operator name, left argument type and schema */
    tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(arrayoper->opno));
    if (! HeapTupleIsValid(tuple))
    {
        elog(ERROR, "cache lookup failed for operator %u", arrayoper->opno);
    }
    opername = pstrdup(((Form_pg_operator)GETSTRUCT(tuple))->oprname.data);
    schema = ((Form_pg_operator)GETSTRUCT(tuple))->oprnamespace;
    ReleaseSysCache(tuple);

    if (schema != PG_CATALOG_NAMESPACE)
        return false;

    /* Accept only constant right sides */
    if (list_length(arrayoper->args) != 2 ||
        nodeTag((Node*) lsecond(arrayoper->args)) != T_Const)
        return false;

    right = (Const*) lsecond(arrayoper->args);

    /* Accept only array constants */
    if (getElementType(right->consttype,1) == right->consttype)
        return false;

    a = DatumGetArrayTypeP(right->constvalue);

    /* Accept only 1-dimensional arrays */
    if (ARR_NDIM(a) != 1)
        return false;

    /* BUG in Quasar: https://slamdata.atlassian.net/browse/SD-1045
     * Quasar can't handle IN (single-value) or IN ()
     */
    if (ARR_DIMS(a)[0] < 2)
        return false;

    if ((strcmp(opername, "=") == 0 && arrayoper->useOr) || /* IN */
        (strcmp(opername, "<>") == 0 && !arrayoper->useOr)) /* NOT IN */
    {
        if (name != NULL) {
            if (strcmp(opername, "=") == 0 && arrayoper->useOr)
                *name = pstrdup("IN");
            else if (strcmp(opername, "<>") == 0 && !arrayoper->useOr)
                *name = pstrdup("NOT IN");
        }

        pfree(opername);
        return true;
    }

    pfree(opername);
    return false;
}

/* Verify if we can pushdown a clause with this column in it
 * This will be true if the column does not have "nopushdown" enabled on it
 * This column can still be SELECTed, but not WHERE'd
*/
bool quasar_can_pushdown_column(int varno, int varattno, PlannerInfo *root) {
    RangeTblEntry *rte;
    List       *options;
    ListCell   *lc;

    /* varno must not be any of OUTER_VAR, INNER_VAR and INDEX_VAR. */
    Assert(!IS_SPECIAL_VARNO(varno));

    /* Get RangeTblEntry from array in PlannerInfo. */
    rte = planner_rt_fetch(varno, root);

    /*
     * If it's a column of a foreign table, and it has the column_name FDW
     * option, use that value.
     */
    options = GetForeignColumnOptions(rte->relid, varattno);
    foreach(lc, options)
    {
        DefElem    *def = (DefElem *) lfirst(lc);

        if (strcmp(def->defname, "nopushdown") == 0)
        {
            return false;
        }
    }

    return true;
}

/*
 * Construct a simple SELECT statement that retrieves desired columns
 * of the specified foreign table, and append it to "buf".  The output
 * contains just "SELECT ... FROM tablename".
 *
 * We also create an integer List of the columns being retrieved, which is
 * returned to *retrieved_attrs.
 */
void
deparseSelectSql(StringInfo buf,
                 PlannerInfo *root,
                 RelOptInfo *baserel,
                 Bitmapset *attrs_used,
                 List **scan_tlist,
                 bool sizeEstimate)
{
    RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);
    Relation        rel;

    /*
     * Core code already has some lock on each rel being planned, so we can
     * use NoLock here.
     */
    rel = heap_open(rte->relid, NoLock);

    /*
     * Construct SELECT list
     */
    appendStringInfoString(buf, "SELECT ");

    if (sizeEstimate)
        appendStringInfoString(buf, "count(*)");
    else
#if(PG_VERSION_NUM >= 90500)
        deparsePushdownTargetList(buf, root, baserel->relid, rel,
                                  baserel->reltargetlist, attrs_used,
                                  scan_tlist);
#else /* PG_VERSION_NUM >= 90500 */
        deparseTargetList(buf, root, baserel->relid, rel, attrs_used);
#endif /* PG_VERSION_NUM >= 90500 */

    /*
     * Construct FROM clause
     */
    appendStringInfoString(buf, " FROM ");
    deparseRelation(buf, rel);

    heap_close(rel, NoLock);
}

#if(PG_VERSION_NUM >= 90500)
/*
 * Emit a target list that retrieves the column expressions
 * as supported by Quasar
 *
 * The tlist text is appended to buf
 */
static void
deparsePushdownTargetList(StringInfo buf,
                          PlannerInfo *root,
                          Index rtindex,
                          Relation rel,
                          List *columnlist,
                          Bitmapset *attrs_used,
                          List **scan_tlist)
{
    /* TODO When 9.5 is stable
       Iterate through columnlist, pulling out expressions
       If they can be executed (foreign_expr_walk),
       append them to scan_tlist
       and run deparseExpr(expr)
       If they cannot be executed, make sure to add any used columns
       in the expression into scan_tlist and run deparseColumnRef */

    TupleDesc       tupdesc = RelationGetDescr(rel);
    bool            have_wholerow;
    bool            first;
    int             i;

    /* If there's a whole-row reference, we'll need all the columns. */
    have_wholerow = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber,
                                  attrs_used);

    first = true;
    for (i = 1; i <= tupdesc->natts; i++)
    {
        Form_pg_attribute attr = tupdesc->attrs[i - 1];

        /* Ignore dropped attributes. */
        if (attr->attisdropped)
            continue;

        if (have_wholerow ||
            bms_is_member(i - FirstLowInvalidHeapAttributeNumber,
                          attrs_used))
        {
            if (!first)
                appendStringInfoString(buf, ", ");
            first = false;

            deparseColumnRef(buf, rtindex, i, root, true);

        }
    }

    /* Don't generate bad syntax if no undropped columns */
    if (first)
        appendStringInfoString(buf, "NULL");
}

#else /* PG_VERSION_NUM >= 90500 */

/*
 * Emit a target list that retrieves the columns specified in attrs_used.
 * This is used for both SELECT and RETURNING targetlists.
 *
 * The tlist text is appended to buf, and we also create an integer List
 * of the columns being retrieved, which is returned to *retrieved_attrs.
 */
static void
deparseTargetList(StringInfo buf,
                  PlannerInfo *root,
                  Index rtindex,
                  Relation rel,
                  Bitmapset *attrs_used)
{
    TupleDesc       tupdesc = RelationGetDescr(rel);
    bool            have_wholerow;
    bool            first;
    int                     i;

    /* If there's a whole-row reference, we'll need all the columns. */
    have_wholerow = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber,
                                  attrs_used);

    first = true;
    for (i = 1; i <= tupdesc->natts; i++)
    {
        Form_pg_attribute attr = tupdesc->attrs[i - 1];

        /* Ignore dropped attributes. */
        if (attr->attisdropped)
            continue;

        if (have_wholerow ||
            bms_is_member(i - FirstLowInvalidHeapAttributeNumber,
                          attrs_used))
        {
            if (!first)
                appendStringInfoString(buf, ", ");
            first = false;

            deparseColumnRef(buf, rtindex, i, root, true);
        }
    }

    /* Don't generate bad syntax if no undropped columns */
    if (first)
        appendStringInfoString(buf, "NULL");
}
#endif /* PG_VERSION_NUM >= 90500 */


/*
 * Deparse WHERE clauses in given list of RestrictInfos and append them to buf.
 *
 * baserel is the foreign table we're planning for.
 *
 * If no WHERE clause already exists in the buffer, is_first should be true.
 *
 * If params is not NULL, it receives a list of Params and other-relation Vars
 * used in the clauses; these values must be transmitted to the remote server
 * as parameter values.
 *
 * If params is NULL, we're generating the query for EXPLAIN purposes,
 * so Params and other-relation Vars should be replaced by dummy values.
 */
void
appendWhereClause(StringInfo buf,
                  PlannerInfo *root,
                  RelOptInfo *baserel,
                  List *exprs,
                  bool is_first,
                  List **params)
{
    deparse_expr_cxt context;
    ListCell   *lc;

    if (params)
        *params = NIL;                  /* initialize result list to empty */

    /* Set up context struct for recursion */
    context.root = root;
    context.foreignrel = baserel;
    context.buf = buf;
    context.params_list = params;

    foreach(lc, exprs)
    {
        RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

        /* Connect expressions with "AND" and parenthesize each condition. */
        if (is_first)
            appendStringInfoString(buf, " WHERE ");
        else
            appendStringInfoString(buf, " AND ");

        appendStringInfoChar(buf, '(');
        deparseExpr(ri->clause, &context);
        appendStringInfoChar(buf, ')');

        is_first = false;
    }

}

/*
 * Construct name to use for given column, and emit it into buf.
 * If it has a column_name FDW option, use that instead of attribute name.
 */
static void
deparseColumnRef(StringInfo buf, int varno, int varattno, PlannerInfo *root, bool selector)
{
    RangeTblEntry *rte;
    char *quasarname = NULL, *pgname;
    List       *options;
    ListCell   *lc;

    /* varno must not be any of OUTER_VAR, INNER_VAR and INDEX_VAR. */
    Assert(!IS_SPECIAL_VARNO(varno));

    /* Get RangeTblEntry from array in PlannerInfo. */
    rte = planner_rt_fetch(varno, root);

    /*
     * If it's a column of a foreign table, and it has the column_name FDW
     * option, use that value.
     */
    options = GetForeignColumnOptions(rte->relid, varattno);
    foreach(lc, options)
    {
        DefElem    *def = (DefElem *) lfirst(lc);

        if (strcmp(def->defname, "map") == 0)
        {
            quasarname = defGetString(def);
            break;
        }
    }

    /* Set the pgname */
    pgname = get_relid_attribute_name(rte->relid, varattno);

    /* If we are in the selector part of the query
     * AND we are mapping the name, emit AS syntax */
    if (selector && quasarname != NULL) {
        appendStringInfo(buf, "%s AS %s", quasar_quote_identifier(quasarname),
                         quasar_quote_identifier(pgname));
    } else {
        appendStringInfoString(
            buf,
            quasar_quote_identifier(quasarname != NULL ? quasarname : pgname));
    }
}

/*
 * Append remote name of specified foreign table to buf.
 * Use value of table_name FDW option (if any) instead of relation's name.
 */
static void
deparseRelation(StringInfo buf, Relation rel)
{
    ForeignTable *table;
    const char *relname = NULL;
    ListCell   *lc;

    /* obtain additional catalog information. */
    table = GetForeignTable(RelationGetRelid(rel));

    /*
     * Use value of FDW options if any, instead of the name of object itself.
     */
    foreach(lc, table->options)
    {
        DefElem    *def = (DefElem *) lfirst(lc);

        if (strcmp(def->defname, "table") == 0)
        {
            char * c;
            relname = defGetString(def);

            /* Skip to the non-path part of the relation */
            c = strrchr(relname, '/');
            if (c != NULL)
                relname = c + 1;
        }
    }

    if (relname == NULL)
        relname = RelationGetRelationName(rel);

    appendStringInfo(buf, "%s",
                     quasar_quote_identifier(relname));
}

/*
 * Append a SQL string literal representing "val" to buf.
 */
void
deparseStringLiteral(StringInfo buf, const char *val)
{
    const char *valptr;

    appendStringInfoChar(buf, '"');
    for (valptr = val; *valptr; valptr++)
    {
        char            ch = *valptr;

        if (SQL_STR_DOUBLE(ch, false))
            appendStringInfoChar(buf, ch);
        appendStringInfoChar(buf, ch);
    }
    appendStringInfoChar(buf, '"');
}

/*
 * Deparse given expression into context->buf.
 *
 * This function must support all the same node types that foreign_expr_walker
 * accepts.
 *
 * Note: unlike ruleutils.c, we just use a simple hard-wired parenthesization
 * scheme: anything more complex than a Var, Const, function call or cast
 * should be self-parenthesized.
 */
static void
deparseExpr(Expr *node, deparse_expr_cxt *context)
{
    if (node == NULL)
        return;

    switch (nodeTag(node))
    {
    case T_Var:
        deparseVar((Var *) node, context);
        break;
    case T_Const:
        deparseConst((Const *) node, context);
        break;
    case T_Param:
        deparseParam((Param *) node, context);
        break;
    case T_ArrayRef:
        deparseArrayRef((ArrayRef *) node, context);
        break;
    case T_FuncExpr:
        deparseFuncExpr((FuncExpr *) node, context);
        break;
    case T_OpExpr:
        deparseOpExpr((OpExpr *) node, context);
        break;
    case T_ScalarArrayOpExpr:
        deparseScalarArrayOpExpr((ScalarArrayOpExpr *) node, context);
        break;
    case T_RelabelType:
        deparseRelabelType((RelabelType *) node, context);
        break;
    case T_BoolExpr:
        deparseBoolExpr((BoolExpr *) node, context);
        break;
    case T_NullTest:
        deparseNullTest((NullTest *) node, context);
        break;
    case T_ArrayExpr:
        deparseArrayExpr((ArrayExpr *) node, context);
        break;
    default:
        elog(ERROR, "unsupported expression type for deparse: %d",
             (int) nodeTag(node));
        break;
    }
}

/*
 * Deparse given Var node into context->buf.
 *
 * If the Var belongs to the foreign relation, just print its remote name.
 * Otherwise, it's effectively a Param (and will in fact be a Param at
 * run time).  Handle it the same way we handle plain Params --- see
 * deparseParam for comments.
 */
static void
deparseVar(Var *node, deparse_expr_cxt *context)
{
    StringInfo      buf = context->buf;

    if (node->varno == context->foreignrel->relid &&
        node->varlevelsup == 0)
    {
        /* Var belongs to foreign table */
        deparseColumnRef(buf, node->varno, node->varattno, context->root, false);
    }
    else
    {
        /* Treat like a Param */
        if (context->params_list)
        {
            int                     pindex = 0;
            ListCell   *lc;

            /* find its index in params_list */
            foreach(lc, *context->params_list)
            {
                pindex++;
                if (equal(node, (Node *) lfirst(lc)))
                    break;
            }
            if (lc == NULL)
            {
                /* not in list, so add it */
                pindex++;
                *context->params_list = lappend(*context->params_list, node);
            }

            printRemoteParam(pindex, node->vartype, node->vartypmod, context);
        }
        else
        {
            printRemotePlaceholder(node->vartype, node->vartypmod, context);
        }
    }
}

/*
 * Deparse given constant value into context->buf.
 *
 * This function has to be kept in sync with ruleutils.c's get_const_expr.
 */
static void
deparseConst(Const *node, deparse_expr_cxt *context)
{
    StringInfo      buf = context->buf;
    Oid                     typoutput;
    bool            typIsVarlena;
    char       *extval;

    if (node->constisnull)
    {
        appendStringInfoString(buf, "NULL");
        return;
    }

    getTypeOutputInfo(node->consttype,
                      &typoutput, &typIsVarlena);
    extval = OidOutputFunctionCall(typoutput, node->constvalue);

    deparseLiteral(buf, node->consttype, extval, node->constvalue);
}

/*
 * Deparse given Param node.
 *
 * If we're generating the query "for real", add the Param to
 * context->params_list if it's not already present, and then use its index
 * in that list as the remote parameter number.  During EXPLAIN, there's
 * no need to identify a parameter number.
 */
static void
deparseParam(Param *node, deparse_expr_cxt *context)
{
    if (context->params_list)
    {
        int                     pindex = 0;
        ListCell   *lc;

        /* find its index in params_list */
        foreach(lc, *context->params_list)
        {
            pindex++;
            if (equal(node, (Node *) lfirst(lc)))
                break;
        }
        if (lc == NULL)
        {
            /* not in list, so add it */
            pindex++;
            *context->params_list = lappend(*context->params_list, node);
        }

        printRemoteParam(pindex, node->paramtype, node->paramtypmod, context);
    }
    else
    {
        printRemotePlaceholder(node->paramtype, node->paramtypmod, context);
    }
}

/*
 * Deparse an array subscript expression.
 */
static void
deparseArrayRef(ArrayRef *node, deparse_expr_cxt *context)
{
    StringInfo      buf = context->buf;
    ListCell   *uplist_item;

    /* Always parenthesize the expression. */
    appendStringInfoChar(buf, '(');

    /*
     * Deparse referenced array expression first.  If that expression includes
     * a cast, we have to parenthesize to prevent the array subscript from
     * being taken as typename decoration.  We can avoid that in the typical
     * case of subscripting a Var, but otherwise do it.
     */
    if (IsA(node->refexpr, Var))
        deparseExpr(node->refexpr, context);
    else
    {
        appendStringInfoChar(buf, '(');
        deparseExpr(node->refexpr, context);
        appendStringInfoChar(buf, ')');
    }

    /* Deparse subscript expressions. */
    /* We've ensured in foreign_expr_walk that these are only
     * single-value subscripts
     */
    foreach(uplist_item, node->refupperindexpr)
    {
        /* Postgres is 1-based but Quasar is 0-based */
        Expr * node = lfirst(uplist_item);
        if (nodeTag(node) == T_Const &&
            IS_INTEGER(((Const *)node)->consttype))
        {
            Const * c = (Const*) node;
            Oid type = c->consttype;
            if (type == INT2OID)
                c->constvalue = Int8GetDatum(DatumGetUInt8(c->constvalue) - 1);
            else if (type == INT4OID)
                c->constvalue = Int16GetDatum(DatumGetInt16(c->constvalue) - 1);
            else
                c->constvalue = Int32GetDatum(DatumGetInt32(c->constvalue) - 1);
            appendStringInfoChar(buf, '[');
            deparseExpr(node, context);
            appendStringInfoChar(buf, ']');
        }
        else
        {
            appendStringInfoString(buf, "[(");
            deparseExpr(node, context);
            appendStringInfoString(buf, "- 1)]");
        }
    }

    appendStringInfoChar(buf, ')');
}

/*
 * Deparse a function call.
 */
static void
deparseFuncExpr(FuncExpr *node, deparse_expr_cxt *context)
{
    StringInfo buf = context->buf;
    char *funcname;
    bool first;
    ListCell *arg;

    /*
     * If the function call came from an implicit coercion, then just show the
     * first argument.
     */
    if (node->funcformat == COERCE_IMPLICIT_CAST)
    {
        deparseExpr((Expr *) linitial(node->args), context);
        return;
    }

    /* Get the quasar function name and deparse */
    quasar_has_function(node, &funcname);
    appendStringInfo(buf, "%s(", quasar_quote_identifier(funcname));

    /* ... and all the arguments */
    first = true;
    foreach(arg, node->args)
    {
        if (!first)
            appendStringInfoString(buf, ", ");
        deparseExpr((Expr *) lfirst(arg), context);
        first = false;
    }
    appendStringInfoChar(buf, ')');

}

/*
 * Deparse given operator expression.   To avoid problems around
 * priority of operations, we always parenthesize the arguments.
 */
static void
deparseOpExpr(OpExpr *node, deparse_expr_cxt *context)
{
    StringInfo      buf = context->buf;
    char *opname;
    char oprkind;
    bool asfunc;
    ListCell   *arg;

    /* Get the quasar operation name */
    quasar_has_op(node, &opname, &oprkind, &asfunc);

    /* Sanity check. */
    Assert((oprkind == 'r' && list_length(node->args) == 1) ||
           (oprkind == 'l' && list_length(node->args) == 1) ||
           (oprkind == 'b' && list_length(node->args) == 2));

    if (!asfunc)
    {
        /* Always parenthesize the expression. */
        appendStringInfoChar(buf, '(');

        /* Deparse left operand. */
        if (oprkind == 'r' || oprkind == 'b')
        {
            arg = list_head(node->args);
            deparseExpr(lfirst(arg), context);
            appendStringInfoChar(buf, ' ');
        }

        /* Insert the operator itself */
        appendStringInfoString(buf, opname);

        /* Deparse right operand. */
        if (oprkind == 'l' || oprkind == 'b')
        {
            arg = list_tail(node->args);
            appendStringInfoChar(buf, ' ');
            deparseExpr(lfirst(arg), context);
        }

        appendStringInfoChar(buf, ')');
    }
    else
    {
        appendStringInfoString(buf, opname);
        appendStringInfoChar(buf, '(');
        deparseExpr(linitial(node->args), context);
        appendStringInfoString(buf, ", ");
        deparseExpr(lsecond(node->args), context);
        appendStringInfoChar(buf, ')');
    }
}

/*
 * Deparse given ScalarArrayOpExpr expression.  To avoid problems
 * around priority of operations, we always parenthesize the arguments.
 */
static void
deparseScalarArrayOpExpr(ScalarArrayOpExpr *node, deparse_expr_cxt *context)
{
    StringInfo buf = context->buf;
    Expr *arg1;
    Const *arg2;
    char *opname;

    quasar_has_scalar_array_op(node, &opname);

    /* Sanity check. */
    Assert(list_length(node->args) == 2);
    Assert(nodeTag((Node*) lsecond(node->args)) == T_ArrayExpr);

    /* Always parenthesize the expression. */
    appendStringInfoChar(buf, '(');

    /* Deparse left operand. */
    arg1 = linitial(node->args);
    deparseExpr(arg1, context);
    appendStringInfoChar(buf, ' ');

    /* Deparse operator name. */
    appendStringInfo(buf, " %s ", opname);

    /* Deparse right operand. */
    arg2 = (Const*) lsecond(node->args);

    /* We dont use the normal deparseExpr here because we need
     * special set syntax */
    deparseArrayLiteral(buf, arg2->constvalue, arg2->consttype,
                        getElementType(arg2->consttype, 1),
                        true);

    /* Always parenthesize the expression. */
    appendStringInfoChar(buf, ')');
}

/*
 * Deparse a RelabelType (binary-compatible cast) node.
 */
static void
deparseRelabelType(RelabelType *node, deparse_expr_cxt *context)
{
    /* Quasar is untyped so don't do anything */
    deparseExpr(node->arg, context);
}

/*
 * Deparse a BoolExpr node.
 */
static void
deparseBoolExpr(BoolExpr *node, deparse_expr_cxt *context)
{
    StringInfo      buf = context->buf;
    const char *op = NULL;          /* keep compiler quiet */
    bool            first;
    ListCell   *lc;

    switch (node->boolop)
    {
    case AND_EXPR:
        op = "AND";
        break;
    case OR_EXPR:
        op = "OR";
        break;
    case NOT_EXPR:
        appendStringInfoString(buf, "(NOT ");
        deparseExpr(linitial(node->args), context);
        appendStringInfoChar(buf, ')');
        return;
    }

    appendStringInfoChar(buf, '(');
    first = true;
    foreach(lc, node->args)
    {
        if (!first)
            appendStringInfo(buf, " %s ", op);
        deparseExpr((Expr *) lfirst(lc), context);
        first = false;
    }
    appendStringInfoChar(buf, ')');
}

/*
 * Deparse IS [NOT] NULL expression.
 */
static void
deparseNullTest(NullTest *node, deparse_expr_cxt *context)
{
    StringInfo      buf = context->buf;

    appendStringInfoChar(buf, '(');
    deparseExpr(node->arg, context);
    if (node->nulltesttype == IS_NULL)
        appendStringInfoString(buf, " IS NULL)");
    else
        appendStringInfoString(buf, " IS NOT NULL)");
}

/*
 * Deparse ARRAY[...] construct.
 */
static void
deparseArrayExpr(ArrayExpr *node, deparse_expr_cxt *context)
{
    StringInfo      buf = context->buf;
    bool            first = true;
    ListCell   *lc;

    appendStringInfoChar(buf, '[');
    foreach(lc, node->elements)
    {
        if (!first)
            appendStringInfoString(buf, ", ");
        deparseExpr(lfirst(lc), context);
        first = false;
    }
    appendStringInfoChar(buf, ']');
}

/*
 * Print the representation of a parameter to be sent to the remote side.
 */
static void
printRemoteParam(int paramindex, Oid paramtype, int32 paramtypmod,
                 deparse_expr_cxt *context)
{
    StringInfo      buf = context->buf;

    appendStringInfo(buf, ":p%d", paramindex);
}

/*
 * Print the representation of a placeholder for a parameter that will be
 * sent to the remote side at execution time.
 *
 * This is used when we're just trying to EXPLAIN the remote query.
 * We don't have the actual value of the runtime parameter yet, and we don't
 * want the remote planner to generate a plan that depends on such a value
 * anyway.  Thus, we can't do something simple like "$1::paramtype".
 * Instead, we emit "((SELECT null::paramtype)::paramtype)".
 * In all extant versions of Postgres, the planner will see that as an unknown
 * constant value, which is what we want.  This might need adjustment if we
 * ever make the planner flatten scalar subqueries.  Note: the reason for the
 * apparently useless outer cast is to ensure that the representation as a
 * whole will be parsed as an a_expr and not a select_with_parens; the latter
 * would do the wrong thing in the context "x = ANY(...)".
 */
static void
printRemotePlaceholder(Oid paramtype, int32 paramtypmod,
                       deparse_expr_cxt *context)
{
    StringInfo      buf = context->buf;

    appendStringInfo(buf, "((SELECT null))");
}

/*
 * Deparse ORDER BY clause according to the given pathkeys for given base
 * relation. From given pathkeys expressions belonging entirely to the given
 * base relation are obtained and deparsed.
 */
void
appendOrderByClause(StringInfo buf, PlannerInfo *root, RelOptInfo *baserel,
                    List *pathkeys)
{
    ListCell   *lcell;
    deparse_expr_cxt context;
    char       *delim = " ";

    /* Set up context struct for recursion */
    context.root = root;
    context.foreignrel = baserel;
    context.buf = buf;
    context.params_list = NULL;

    appendStringInfo(buf, " ORDER BY");
    foreach(lcell, pathkeys)
    {
        PathKey    *pathkey = lfirst(lcell);
        Expr       *em_expr;

        em_expr = find_em_expr_for_rel(pathkey->pk_eclass, baserel);
        Assert(em_expr != NULL);

        appendStringInfoString(buf, delim);
        deparseExpr(em_expr, &context);
        if (pathkey->pk_strategy == BTLessStrategyNumber)
            appendStringInfoString(buf, " ASC");
        else
            appendStringInfoString(buf, " DESC");

        delim = ", ";
    }
}


/* Quasar Identifier auto-quoter.
 * transforms `top[*].mid.bot[0]` to `"top"[*]."mid"."bot"[0]`
 */
typedef enum {
    QUOTE_STATE_DEFAULT,
    QUOTE_STATE_IN_QUOTE,
    QUOTE_STATE_IN_ARRAYREF
} QuasarQuoteState;

static char *
quasar_quote_identifier(const char *s) {
    QuasarQuoteState state = QUOTE_STATE_DEFAULT;
    StringInfoData buf;
    initStringInfo(&buf);

    for (; *s != '\0'; ++s)
    {
        switch(state) {
        case QUOTE_STATE_DEFAULT:
            switch(*s) {
            case '.':
                break;
            case '[':
                state = QUOTE_STATE_IN_ARRAYREF;
                break;
            case '`':
                /* User self-quoting */
                state = QUOTE_STATE_IN_QUOTE;
                break;
            default:
                /* We have encountered a token to quote */
                state = QUOTE_STATE_IN_QUOTE;
                appendStringInfoChar(&buf, '`');
            }
            break;
        case QUOTE_STATE_IN_QUOTE:
            switch(*s) {
            case '.':
                state = QUOTE_STATE_DEFAULT;
                appendStringInfoChar(&buf, '`');
                break;
            case '[':
                state = QUOTE_STATE_IN_ARRAYREF;
                appendStringInfoChar(&buf, '`');
                break;
            case '`':
                state = QUOTE_STATE_DEFAULT;
                break;
            }
            break;
        case QUOTE_STATE_IN_ARRAYREF:
            switch(*s) {
            case ']':
                state = QUOTE_STATE_DEFAULT;
                break;
            }
            break;
        }
        appendStringInfoChar(&buf, *s);
    }

    if (state == QUOTE_STATE_IN_QUOTE)
        appendStringInfoChar(&buf, '`');

    return buf.data;
}

extern void
deparseLiteral(StringInfo buf, Oid type, const char *svalue, Datum value)
{
    switch (type)
    {
    case INT2OID:
    case INT4OID:
    case INT8OID:
    case OIDOID:
    case FLOAT4OID:
    case FLOAT8OID:
    case NUMERICOID:
    {
        /* Check for NaN */
        if (strspn(svalue, "0123456789+-eE.") == strlen(svalue))
        {
            if (svalue[0] == '+' || svalue[0] == '-')
                appendStringInfo(buf, "(%s)", svalue);
            else
                appendStringInfoString(buf, svalue);
        }
        else
            /* Quasar cannot handle 'NaN', so we emit null */
            appendStringInfoString(buf, "NULL");
    }
    break;
    case BOOLOID:
        if (strcmp(svalue, "t") == 0)
            appendStringInfoString(buf, "true");
        else
            appendStringInfoString(buf, "false");
        break;
    case TIMEOID:
    {
        appendStringInfo(buf, "TIME(\"%s\")", svalue);
    }
    break;
    case DATEOID:
    {
        struct pg_tm tm;
        fsec_t fsec;

        if (timestamp2tm(date2timestamp_no_overflow(DatumGetDateADT(value)),
                         NULL, &tm, &fsec, NULL, NULL))
        {
            elog(ERROR, "quasar_fdw: Couldn't convert date value to pg_tm struct");
        }
        appendStringInfo(buf, "DATE(\"%04d-%02d-%02d\")",
                         tm.tm_year, tm.tm_mon, tm.tm_mday);
    }
    break;
    case TIMESTAMPOID:
    {
        struct pg_tm tm;
        fsec_t fsec;

        if (timestamp2tm(DatumGetTimestamp(value), NULL, &tm, &fsec, NULL, NULL))
        {
            elog(ERROR, "quasar_fdw: Couldn't convert timestamp to pg_tm struct");
        }

        appendStringInfo(buf, "TIMESTAMP(\"%04d-%02d-%02dT%02d:%02d:%02dZ\")",
                         tm.tm_year, tm.tm_mon, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    break;
    case TIMESTAMPTZOID:
    {
        pg_time_t tt = timestamptz_to_time_t(DatumGetTimestampTz(value));
        struct pg_tm *tm = pg_gmtime(&tt);
        appendStringInfo(buf, "TIMESTAMP(\"%04d-%02d-%02dT%02d:%02d:%02dZ\")",
                         tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                         tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    break;
    case INTERVALOID:
    {
        Interval *span = DatumGetIntervalP(value);
        struct pg_tm tm;
        fsec_t fsec;

        if (interval2tm(*span, &tm, &fsec))
        {
            elog(ERROR, "quasar_fdw: Couldn't convert interval to pg_tm struct");
        }

        appendStringInfo(buf, "INTERVAL(\"P%dDT%dH%dM%dS\")",
                         tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    break;
    default:
    {
        Oid elemType = getElementType(type, 1);
        if (elemType != type) {
            deparseArrayLiteral(buf, value, type, elemType, false);
        } else {
            deparseStringLiteral(buf, svalue);
        }
    }
    break;
    }
}

void deparseArrayLiteral(StringInfo buf, Datum value, Oid type, Oid elemType, bool use_set_syntax)
{
    ArrayIterator iterator;
    bool first = true;
    Datum datum;
    bool isNull;
    Oid  typoutput;
    bool typIsVarlena;
    char *svalue;

    getTypeOutputInfo(elemType, &typoutput, &typIsVarlena);

    appendStringInfoChar(buf, use_set_syntax ? '(' : '[');

    /* loop through the array elements */
#if(PG_VERSION_NUM >= 90500)
    iterator = array_create_iterator(DatumGetArrayTypeP(value), 0, NULL);
#else
    iterator = array_create_iterator(DatumGetArrayTypeP(value), 0);
#endif
    while (array_iterate(iterator, &datum, &isNull))
    {
        if (!first)
        {
            appendStringInfoString(buf, ", ");
        }
        first = false;

        if (isNull)
            appendStringInfoString(buf, "NULL");
        else
        {
            svalue = OidOutputFunctionCall(typoutput, datum);
            deparseLiteral(buf, elemType, svalue, value);
        }
    }
    array_free_iterator(iterator);

    appendStringInfoChar(buf, use_set_syntax ? ')' : ']');
}
