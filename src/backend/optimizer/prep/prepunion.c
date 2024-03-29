/*-------------------------------------------------------------------------
 *
 * prepunion.c
 *      Routines to plan set-operation queries.  The filename is a leftover
 *      from a time when only UNIONs were implemented.
 *
 * There are two code paths in the planner for set-operation queries.
 * If a subquery consists entirely of simple UNION ALL operations, it
 * is converted into an "append relation".  Otherwise, it is handled
 * by the general code in this module (plan_set_operations and its
 * subroutines).  There is some support code here for the append-relation
 * case, but most of the heavy lifting for that is done elsewhere,
 * notably in prepjointree.c and allpaths.c.
 *
 * There is also some code here to support planning of queries that use
 * inheritance (SELECT FROM foo*).  Inheritance trees are converted into
 * append relations, and thenceforth share code with the UNION ALL case.
 *
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *      src/backend/optimizer/prep/prepunion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"


typedef struct
{
    PlannerInfo *root;
	int nappinfos;
	AppendRelInfo **appinfos;
} adjust_appendrel_attrs_context;

static Path *recurse_set_operations(Node *setOp, PlannerInfo *root,
                       List *colTypes, List *colCollations,
                       bool junkOK,
                       int flag, List *refnames_tlist,
                       List **pTargetList,
                       double *pNumGroups);
static Path *generate_recursion_path(SetOperationStmt *setOp,
                        PlannerInfo *root,
                        List *refnames_tlist,
                        List **pTargetList);
static Path *generate_union_path(SetOperationStmt *op, PlannerInfo *root,
                    List *refnames_tlist,
                    List **pTargetList,
                    double *pNumGroups);
static Path *generate_nonunion_path(SetOperationStmt *op, PlannerInfo *root,
                       List *refnames_tlist,
                       List **pTargetList,
                       double *pNumGroups);
static List *recurse_union_children(Node *setOp, PlannerInfo *root,
                       SetOperationStmt *top_union,
                       List *refnames_tlist,
                       List **tlist_list);
static Path *make_union_unique(SetOperationStmt *op, Path *path, List *tlist,
                  PlannerInfo *root);
static bool choose_hashed_setop(PlannerInfo *root, List *groupClauses,
                    Path *input_path,
                    double dNumGroups, double dNumOutputRows,
                    const char *construct);
static List *generate_setop_tlist(List *colTypes, List *colCollations,
                     int flag,
                     Index varno,
                     bool hack_constants,
                     List *input_tlist,
                     List *refnames_tlist);
static List *generate_append_tlist(List *colTypes, List *colCollations,
                      bool flag,
                      List *input_tlists,
                      List *refnames_tlist);
static List *generate_setop_grouplist(SetOperationStmt *op, List *targetlist);
static void expand_inherited_rtentry(PlannerInfo *root, RangeTblEntry *rte,
                         Index rti);
static void expand_partitioned_rtentry(PlannerInfo *root,
						   RangeTblEntry *parentrte,
						   Index parentRTindex, Relation parentrel,
						   PlanRowMark *top_parentrc, LOCKMODE lockmode,
						   List **appinfos);
static void expand_single_inheritance_child(PlannerInfo *root,
								RangeTblEntry *parentrte,
								Index parentRTindex, Relation parentrel,
								PlanRowMark *top_parentrc, Relation childrel,
								List **appinfos, RangeTblEntry **childrte_p,
								Index *childRTindex_p);
static void make_inh_translation_list(Relation oldrelation,
                          Relation newrelation,
                          Index newvarno,
                          List **translated_vars);
static Bitmapset *translate_col_privs(const Bitmapset *parent_privs,
                    List *translated_vars);
static Node *adjust_appendrel_attrs_mutator(Node *node,
                               adjust_appendrel_attrs_context *context);
static Relids adjust_child_relids(Relids relids, int nappinfos,
                    AppendRelInfo **appinfos);
static Relids adjust_child_relids(Relids relids, int nappinfos,
                   AppendRelInfo **appinfos);
static List *adjust_inherited_tlist(List *tlist,
                       AppendRelInfo *context);


/*
 * plan_set_operations
 *
 *      Plans the queries for a tree of set operations (UNION/INTERSECT/EXCEPT)
 *
 * This routine only deals with the setOperations tree of the given query.
 * Any top-level ORDER BY requested in root->parse->sortClause will be handled
 * when we return to grouping_planner; likewise for LIMIT.
 *
 * What we return is an "upperrel" RelOptInfo containing at least one Path
 * that implements the set-operation tree.  In addition, root->processed_tlist
 * receives a targetlist representing the output of the topmost setop node.
 */
RelOptInfo *
plan_set_operations(PlannerInfo *root)
{
    Query       *parse = root->parse;
    SetOperationStmt *topop = castNode(SetOperationStmt, parse->setOperations);
    Node       *node;
    RangeTblEntry *leftmostRTE;
    Query       *leftmostQuery;
    RelOptInfo *setop_rel;
    Path       *path;
    List       *top_tlist;

    Assert(topop);

    /* check for unsupported stuff */
    Assert(parse->jointree->fromlist == NIL);
    Assert(parse->jointree->quals == NULL);
    Assert(parse->groupClause == NIL);
    Assert(parse->havingQual == NULL);
    Assert(parse->windowClause == NIL);
    Assert(parse->distinctClause == NIL);

    /*
     * We'll need to build RelOptInfos for each of the leaf subqueries, which
     * are RTE_SUBQUERY rangetable entries in this Query.  Prepare the index
     * arrays for that.
     */
    setup_simple_rel_arrays(root);

    /*
     * Find the leftmost component Query.  We need to use its column names for
     * all generated tlists (else SELECT INTO won't work right).
     */
    node = topop->larg;
    while (node && IsA(node, SetOperationStmt))
        node = ((SetOperationStmt *) node)->larg;
    Assert(node && IsA(node, RangeTblRef));
    leftmostRTE = root->simple_rte_array[((RangeTblRef *) node)->rtindex];
    leftmostQuery = leftmostRTE->subquery;
    Assert(leftmostQuery != NULL);

    /*
     * We return our results in the (SETOP, NULL) upperrel.  For the moment,
     * this is also the parent rel of all Paths in the setop tree; we may well
     * change that in future.
     */
    setop_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);

    /*
     * We don't currently worry about setting setop_rel's consider_parallel
     * flag, nor about allowing FDWs to contribute paths to it.
     */

    /*
     * If the topmost node is a recursive union, it needs special processing.
     */
    if (root->hasRecursion)
    {
        path = generate_recursion_path(topop, root,
                                       leftmostQuery->targetList,
                                       &top_tlist);
    }
    else
    {
        /*
         * Recurse on setOperations tree to generate paths for set ops. The
         * final output path should have just the column types shown as the
         * output from the top-level node, plus possibly resjunk working
         * columns (we can rely on upper-level nodes to deal with that).
         */
        path = recurse_set_operations((Node *) topop, root,
                                      topop->colTypes, topop->colCollations,
                                      true, -1,
                                      leftmostQuery->targetList,
                                      &top_tlist,
                                      NULL);
    }

    /* Must return the built tlist into root->processed_tlist. */
    root->processed_tlist = top_tlist;

    /* Add only the final path to the SETOP upperrel. */
    add_path(setop_rel, path);

    /* Let extensions possibly add some more paths */
    if (create_upper_paths_hook)
        (*create_upper_paths_hook) (root, UPPERREL_SETOP,
                                    NULL, setop_rel);

    /* Select cheapest path */
    set_cheapest(setop_rel);

    return setop_rel;
}

/*
 * recurse_set_operations
 *      Recursively handle one step in a tree of set operations
 *
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * junkOK: if true, child resjunk columns may be left in the result
 * flag: if >= 0, add a resjunk output column indicating value of flag
 * refnames_tlist: targetlist to take column names from
 *
 * Returns a path for the subtree, as well as these output parameters:
 * *pTargetList: receives the fully-fledged tlist for the subtree's top plan
 * *pNumGroups: if not NULL, we estimate the number of distinct groups
 *        in the result, and store it there
 *
 * The pTargetList output parameter is mostly redundant with the pathtarget
 * of the returned path, but for the moment we need it because much of the
 * logic in this file depends on flag columns being marked resjunk.  Pending
 * a redesign of how that works, this is the easy way out.
 *
 * We don't have to care about typmods here: the only allowed difference
 * between set-op input and output typmods is input is a specific typmod
 * and output is -1, and that does not require a coercion.
 */
static Path *
recurse_set_operations(Node *setOp, PlannerInfo *root,
                       List *colTypes, List *colCollations,
                       bool junkOK,
                       int flag, List *refnames_tlist,
                       List **pTargetList,
                       double *pNumGroups)
{// #lizard forgives
    if (IsA(setOp, RangeTblRef))
    {
        RangeTblRef *rtr = (RangeTblRef *) setOp;
        RangeTblEntry *rte = root->simple_rte_array[rtr->rtindex];
        Query       *subquery = rte->subquery;
        RelOptInfo *rel;
        PlannerInfo *subroot;
        RelOptInfo *final_rel;
        Path       *subpath;
        Path       *path;
        List       *tlist;

        Assert(subquery != NULL);

        /*
         * We need to build a RelOptInfo for each leaf subquery.  This isn't
         * used for much here, but it carries the subroot data structures
         * forward to setrefs.c processing.
         */
        rel = build_simple_rel(root, rtr->rtindex, NULL);

        /* plan_params should not be in use in current query level */
        Assert(root->plan_params == NIL);

        /* Generate a subroot and Paths for the subquery */
        subroot = rel->subroot = subquery_planner(root->glob, subquery,
                                                  root,
                                                  false,
                                                  root->tuple_fraction);

        /* Save subroot and subplan in RelOptInfo for setrefs.c */
        rel->subroot = subroot;

        if (root->recursiveOk)    
            root->recursiveOk = subroot->recursiveOk;

        /*
         * It should not be possible for the primitive query to contain any
         * cross-references to other primitive queries in the setop tree.
         */
        if (root->plan_params)
            elog(ERROR, "unexpected outer reference in set operation subquery");

        /*
         * Mark rel with estimated output rows, width, etc.  Note that we have
         * to do this before generating outer-query paths, else
         * cost_subqueryscan is not happy.
         */
        set_subquery_size_estimates(root, rel);

        /*
         * For the moment, we consider only a single Path for the subquery.
         * This should change soon (make it look more like
         * set_subquery_pathlist).
         */
        final_rel = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
        subpath = get_cheapest_fractional_path(final_rel,
                                               root->tuple_fraction);

#ifdef XCP
        /*
          * create remote_subplan_path if needed, and we'll use this path to
          * create remote_subplan at the top.
          */
        if(subpath->distribution)
        {
            subpath = create_remotesubplan_path(NULL, subpath, NULL);

            subroot->distribution = NULL;
        }
#endif

        /*
         * Stick a SubqueryScanPath atop that.
         *
         * We don't bother to determine the subquery's output ordering since
         * it won't be reflected in the set-op result anyhow; so just label
         * the SubqueryScanPath with nil pathkeys.  (XXX that should change
         * soon too, likely.)
         */
        path = (Path *) create_subqueryscan_path(root, rel, subpath,
                                                 NIL, NULL, subroot->distribution);

        /*
         * Figure out the appropriate target list, and update the
         * SubqueryScanPath with the PathTarget form of that.
         */
        tlist = generate_setop_tlist(colTypes, colCollations,
                                     flag,
                                     rtr->rtindex,
                                     true,
                                     subroot->processed_tlist,
                                     refnames_tlist);

        path = apply_projection_to_path(root, rel, path,
                                        create_pathtarget(root, tlist));

        /* Return the fully-fledged tlist to caller, too */
        *pTargetList = tlist;

        /*
         * Estimate number of groups if caller wants it.  If the subquery used
         * grouping or aggregation, its output is probably mostly unique
         * anyway; otherwise do statistical estimation.
         *
         * XXX you don't really want to know about this: we do the estimation
         * using the subquery's original targetlist expressions, not the
         * subroot->processed_tlist which might seem more appropriate.  The
         * reason is that if the subquery is itself a setop, it may return a
         * processed_tlist containing "varno 0" Vars generated by
         * generate_append_tlist, and those would confuse estimate_num_groups
         * mightily.  We ought to get rid of the "varno 0" hack, but that
         * requires a redesign of the parsetree representation of setops, so
         * that there can be an RTE corresponding to each setop's output.
         */
        if (pNumGroups)
        {
            if (subquery->groupClause || subquery->groupingSets ||
                subquery->distinctClause ||
                subroot->hasHavingQual || subquery->hasAggs)
                *pNumGroups = subpath->rows;
            else
                *pNumGroups = estimate_num_groups(subroot,
                                                  get_tlist_exprs(subquery->targetList, false),
                                                  subpath->rows,
                                                  NULL);
        }

        return (Path *) path;
    }
    else if (IsA(setOp, SetOperationStmt))
    {
        SetOperationStmt *op = (SetOperationStmt *) setOp;
        Path       *path;

        /* UNIONs are much different from INTERSECT/EXCEPT */
        if (op->op == SETOP_UNION)
            path = generate_union_path(op, root,
                                       refnames_tlist,
                                       pTargetList,
                                       pNumGroups);
        else
            path = generate_nonunion_path(op, root,
                                          refnames_tlist,
                                          pTargetList,
                                          pNumGroups);

        /*
         * If necessary, add a Result node to project the caller-requested
         * output columns.
         *
         * XXX you don't really want to know about this: setrefs.c will apply
         * fix_upper_expr() to the Result node's tlist. This would fail if the
         * Vars generated by generate_setop_tlist() were not exactly equal()
         * to the corresponding tlist entries of the subplan. However, since
         * the subplan was generated by generate_union_plan() or
         * generate_nonunion_plan(), and hence its tlist was generated by
         * generate_append_tlist(), this will work.  We just tell
         * generate_setop_tlist() to use varno 0.
         */
        if (flag >= 0 ||
            !tlist_same_datatypes(*pTargetList, colTypes, junkOK) ||
            !tlist_same_collations(*pTargetList, colCollations, junkOK))
        {
            *pTargetList = generate_setop_tlist(colTypes, colCollations,
                                                flag,
                                                0,
                                                false,
                                                *pTargetList,
                                                refnames_tlist);
            path = apply_projection_to_path(root,
                                            path->parent,
                                            path,
                                            create_pathtarget(root,
                                                              *pTargetList));
        }
        return path;
    }
    else
    {
        elog(ERROR, "unrecognized node type: %d",
             (int) nodeTag(setOp));
        *pTargetList = NIL;
        return NULL;            /* keep compiler quiet */
    }
}

/*
 * remove RemoteSubquery from the top of the path
 *
 * Essentially find_push_down_plan() but applied when constructing the path,
 * not when creating the plan. Compared to find_push_down_plan it only deals
 * with a subset of node types, however.
 *
 * XXX Does this need to handle additional node types?
 */
static Path *
strip_remote_subquery(PlannerInfo *root, Path *path)
{
    /* if there's RemoteSubplan at the top, we're trivially done */
    if (IsA(path, RemoteSubPath))
        return ((RemoteSubPath *)path)->subpath;

    /* for subquery, we tweak the subpath (and descend into it) */
    if (IsA(path, SubqueryScanPath))
    {
        SubqueryScanPath *subquery = (SubqueryScanPath *)path;
        subquery->subpath = strip_remote_subquery(root, subquery->subpath);

        subquery->path.param_info = subquery->subpath->param_info;
        subquery->path.pathkeys = subquery->subpath->pathkeys;

        /* also update the distribution */
        subquery->path.distribution = copyObject(subquery->subpath->distribution);

        /* recompute costs */
        cost_subqueryscan(subquery, root, path->parent, subquery->path.param_info);
    }

    return path;
}

/*
 * Generate path for a recursive UNION node
 */
static Path *
generate_recursion_path(SetOperationStmt *setOp, PlannerInfo *root,
                        List *refnames_tlist,
                        List **pTargetList)
{
    RelOptInfo *result_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);
    Path       *path;
    Path       *lpath;
    Path       *rpath;
    List       *lpath_tlist;
    List       *rpath_tlist;
    List       *tlist;
    List       *groupList;
    double        dNumGroups;

    /* Parser should have rejected other cases */
    if (setOp->op != SETOP_UNION)
        elog(ERROR, "only UNION queries can be recursive");
    /* Worktable ID should be assigned */
    Assert(root->wt_param_id >= 0);

    /*
     * Unlike a regular UNION node, process the left and right inputs
     * separately without any intention of combining them into one Append.
     */
    lpath = recurse_set_operations(setOp->larg, root,
                                   setOp->colTypes, setOp->colCollations,
                                   false, -1,
                                   refnames_tlist,
                                   &lpath_tlist,
                                   NULL);
    /* The right path will want to look at the left one ... */
    root->non_recursive_path = lpath;
    rpath = recurse_set_operations(setOp->rarg, root,
                                   setOp->colTypes, setOp->colCollations,
                                   false, -1,
                                   refnames_tlist,
                                   &rpath_tlist,
                                   NULL);
    root->non_recursive_path = NULL;

    /*
     * Generate tlist for RecursiveUnion path node --- same as in Append cases
     */
    tlist = generate_append_tlist(setOp->colTypes, setOp->colCollations, false,
                                  list_make2(lpath_tlist, rpath_tlist),
                                  refnames_tlist);

    *pTargetList = tlist;

    /*
     * If UNION, identify the grouping operators
     */
    if (setOp->all)
    {
        groupList = NIL;
        dNumGroups = 0;
    }
    else
    {
        /* Identify the grouping semantics */
        groupList = generate_setop_grouplist(setOp, tlist);

        /* We only support hashing here */
        if (!grouping_is_hashable(groupList))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("could not implement recursive UNION"),
                     errdetail("All column datatypes must be hashable.")));

        /*
         * For the moment, take the number of distinct groups as equal to the
         * total input size, ie, the worst case.
         */
        dNumGroups = lpath->rows + rpath->rows * 10;
    }

    /*
     * Push the resursive union (CTE) below Remote Subquery.
     *
     * We have already checked that all tables involved in the recursive CTE
     * are replicated tables (or coordinator local tables such as catalogs).
     * See subquery_planner for details. So here we search the left and right
     * subpaths, and search for those remote subqueries.
     *
     * If either side contains a remote subquery, we remove those, and instead
     * add a remote subquery on top of the recursive union later (we don't need
     * to do that manually, it'll happen automatically).
     *
     * XXX The tables may be marked for execution on different nodes, but that
     * does not matter since tables are replicated, and execution nodes are
     * picked randomly.
     *
     * XXX For tables replicated on different groups of nodes, this may not
     * work. We either need to pick a node from an intersection of the groups,
     * or simply disable recursive queries on such tables.
     *
     * XXX This obviously breaks costing, because we're removing nodes that
     * affected the cost (network transfers).
     */
    rpath = strip_remote_subquery(root, rpath);
    lpath = strip_remote_subquery(root, lpath);

    /*
     * And make the path node.
     */
    path = (Path *) create_recursiveunion_path(root,
                                               result_rel,
                                               lpath,
                                               rpath,
                                               create_pathtarget(root, tlist),
                                               groupList,
                                               root->wt_param_id,
                                               dNumGroups);

    return path;
}

/*
 * Generate path for a UNION or UNION ALL node
 */
static Path *
generate_union_path(SetOperationStmt *op, PlannerInfo *root,
                    List *refnames_tlist,
                    List **pTargetList,
                    double *pNumGroups)
{
    RelOptInfo *result_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);
    double        save_fraction = root->tuple_fraction;
    List       *pathlist;
    List       *child_tlists1;
    List       *child_tlists2;
    List       *tlist_list;
    List       *tlist;
    Path       *path;

    /*
     * If plain UNION, tell children to fetch all tuples.
     *
     * Note: in UNION ALL, we pass the top-level tuple_fraction unmodified to
     * each arm of the UNION ALL.  One could make a case for reducing the
     * tuple fraction for later arms (discounting by the expected size of the
     * earlier arms' results) but it seems not worth the trouble. The normal
     * case where tuple_fraction isn't already zero is a LIMIT at top level,
     * and passing it down as-is is usually enough to get the desired result
     * of preferring fast-start plans.
     */
    if (!op->all)
        root->tuple_fraction = 0.0;

    /*
     * If any of my children are identical UNION nodes (same op, all-flag, and
     * colTypes) then they can be merged into this node so that we generate
     * only one Append and unique-ification for the lot.  Recurse to find such
     * nodes and compute their children's paths.
     */
    pathlist = list_concat(recurse_union_children(op->larg, root,
                                                  op, refnames_tlist,
                                                  &child_tlists1),
                           recurse_union_children(op->rarg, root,
                                                  op, refnames_tlist,
                                                  &child_tlists2));
    tlist_list = list_concat(child_tlists1, child_tlists2);

    /*
     * Generate tlist for Append plan node.
     *
     * The tlist for an Append plan isn't important as far as the Append is
     * concerned, but we must make it look real anyway for the benefit of the
     * next plan level up.
     */
    tlist = generate_append_tlist(op->colTypes, op->colCollations, false,
                                  tlist_list, refnames_tlist);

    *pTargetList = tlist;

    /*
     * Append the child results together.
     */
    path = (Path *) create_append_path(result_rel, pathlist, NULL, 0, NIL);

    /* We have to manually jam the right tlist into the path; ick */
    path->pathtarget = create_pathtarget(root, tlist);

    /*
     * For UNION ALL, we just need the Append path.  For UNION, need to add
     * node(s) to remove duplicates.
     */
    if (!op->all)
        path = make_union_unique(op, path, tlist, root);

    /*
     * Estimate number of groups if caller wants it.  For now we just assume
     * the output is unique --- this is certainly true for the UNION case, and
     * we want worst-case estimates anyway.
     */
    if (pNumGroups)
        *pNumGroups = path->rows;

    /* Undo effects of possibly forcing tuple_fraction to 0 */
    root->tuple_fraction = save_fraction;

    return path;
}

/*
 * Generate path for an INTERSECT, INTERSECT ALL, EXCEPT, or EXCEPT ALL node
 */
static Path *
generate_nonunion_path(SetOperationStmt *op, PlannerInfo *root,
                       List *refnames_tlist,
                       List **pTargetList,
                       double *pNumGroups)
{// #lizard forgives
    RelOptInfo *result_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);
    double        save_fraction = root->tuple_fraction;
    Path       *lpath,
               *rpath,
               *path;
    List       *lpath_tlist,
               *rpath_tlist,
               *tlist_list,
               *tlist,
               *groupList,
               *pathlist;
    double        dLeftGroups,
                dRightGroups,
                dNumGroups,
                dNumOutputRows;
    bool        use_hash;
    SetOpCmd    cmd;
    int            firstFlag;

    /*
     * Tell children to fetch all tuples.
     */
    root->tuple_fraction = 0.0;

    /* Recurse on children, ensuring their outputs are marked */
    lpath = recurse_set_operations(op->larg, root,
                                   op->colTypes, op->colCollations,
                                   false, 0,
                                   refnames_tlist,
                                   &lpath_tlist,
                                   &dLeftGroups);
    rpath = recurse_set_operations(op->rarg, root,
                                   op->colTypes, op->colCollations,
                                   false, 1,
                                   refnames_tlist,
                                   &rpath_tlist,
                                   &dRightGroups);

    /* Undo effects of forcing tuple_fraction to 0 */
    root->tuple_fraction = save_fraction;

    /*
     * For EXCEPT, we must put the left input first.  For INTERSECT, either
     * order should give the same results, and we prefer to put the smaller
     * input first in order to minimize the size of the hash table in the
     * hashing case.  "Smaller" means the one with the fewer groups.
     */
    if (op->op == SETOP_EXCEPT || dLeftGroups <= dRightGroups)
    {
        pathlist = list_make2(lpath, rpath);
        tlist_list = list_make2(lpath_tlist, rpath_tlist);
        firstFlag = 0;
    }
    else
    {
        pathlist = list_make2(rpath, lpath);
        tlist_list = list_make2(rpath_tlist, lpath_tlist);
        firstFlag = 1;
    }

    /*
     * Generate tlist for Append plan node.
     *
     * The tlist for an Append plan isn't important as far as the Append is
     * concerned, but we must make it look real anyway for the benefit of the
     * next plan level up.  In fact, it has to be real enough that the flag
     * column is shown as a variable not a constant, else setrefs.c will get
     * confused.
     */
    tlist = generate_append_tlist(op->colTypes, op->colCollations, true,
                                  tlist_list, refnames_tlist);

    *pTargetList = tlist;

    /*
     * Append the child results together.
     */
    path = (Path *) create_append_path(result_rel, pathlist, NULL, 0, NIL);

    /* We have to manually jam the right tlist into the path; ick */
    path->pathtarget = create_pathtarget(root, tlist);

    /* Identify the grouping semantics */
    groupList = generate_setop_grouplist(op, tlist);

    /* punt if nothing to group on (can this happen?) */
    if (groupList == NIL)
        return path;

    /*
     * Estimate number of distinct groups that we'll need hashtable entries
     * for; this is the size of the left-hand input for EXCEPT, or the smaller
     * input for INTERSECT.  Also estimate the number of eventual output rows.
     * In non-ALL cases, we estimate each group produces one output row; in
     * ALL cases use the relevant relation size.  These are worst-case
     * estimates, of course, but we need to be conservative.
     */
    if (op->op == SETOP_EXCEPT)
    {
        dNumGroups = dLeftGroups;
        dNumOutputRows = op->all ? lpath->rows : dNumGroups;
    }
    else
    {
        dNumGroups = Min(dLeftGroups, dRightGroups);
        dNumOutputRows = op->all ? Min(lpath->rows, rpath->rows) : dNumGroups;
    }

    /*
     * Decide whether to hash or sort, and add a sort node if needed.
     */
    use_hash = choose_hashed_setop(root, groupList, path,
                                   dNumGroups, dNumOutputRows,
                                   (op->op == SETOP_INTERSECT) ? "INTERSECT" : "EXCEPT");

    if (!use_hash)
        path = (Path *) create_sort_path(root,
                                         result_rel,
                                         path,
                                         make_pathkeys_for_sortclauses(root,
                                                                       groupList,
                                                                       tlist),
                                         -1.0);

    /*
     * Finally, add a SetOp path node to generate the correct output.
     */
    switch (op->op)
    {
        case SETOP_INTERSECT:
            cmd = op->all ? SETOPCMD_INTERSECT_ALL : SETOPCMD_INTERSECT;
            break;
        case SETOP_EXCEPT:
            cmd = op->all ? SETOPCMD_EXCEPT_ALL : SETOPCMD_EXCEPT;
            break;
        default:
            elog(ERROR, "unrecognized set op: %d", (int) op->op);
            cmd = SETOPCMD_INTERSECT;    /* keep compiler quiet */
            break;
    }
    path = (Path *) create_setop_path(root,
                                      result_rel,
                                      path,
                                      cmd,
                                      use_hash ? SETOP_HASHED : SETOP_SORTED,
                                      groupList,
                                      list_length(op->colTypes) + 1,
                                      use_hash ? firstFlag : -1,
                                      dNumGroups,
                                      dNumOutputRows);

    if (pNumGroups)
        *pNumGroups = dNumGroups;

    return path;
}

/*
 * Pull up children of a UNION node that are identically-propertied UNIONs.
 *
 * NOTE: we can also pull a UNION ALL up into a UNION, since the distinct
 * output rows will be lost anyway.
 *
 * NOTE: currently, we ignore collations while determining if a child has
 * the same properties.  This is semantically sound only so long as all
 * collations have the same notion of equality.  It is valid from an
 * implementation standpoint because we don't care about the ordering of
 * a UNION child's result: UNION ALL results are always unordered, and
 * generate_union_path will force a fresh sort if the top level is a UNION.
 */
static List *
recurse_union_children(Node *setOp, PlannerInfo *root,
                       SetOperationStmt *top_union,
                       List *refnames_tlist,
                       List **tlist_list)
{
    List       *result;
    List       *child_tlist;

    if (IsA(setOp, SetOperationStmt))
    {
        SetOperationStmt *op = (SetOperationStmt *) setOp;

        if (op->op == top_union->op &&
            (op->all == top_union->all || op->all) &&
            equal(op->colTypes, top_union->colTypes))
        {
            /* Same UNION, so fold children into parent's subpath list */
            List       *child_tlists1;
            List       *child_tlists2;

            result = list_concat(recurse_union_children(op->larg, root,
                                                        top_union,
                                                        refnames_tlist,
                                                        &child_tlists1),
                                 recurse_union_children(op->rarg, root,
                                                        top_union,
                                                        refnames_tlist,
                                                        &child_tlists2));
            *tlist_list = list_concat(child_tlists1, child_tlists2);
            return result;
        }
    }

    /*
     * Not same, so plan this child separately.
     *
     * Note we disallow any resjunk columns in child results.  This is
     * necessary since the Append node that implements the union won't do any
     * projection, and upper levels will get confused if some of our output
     * tuples have junk and some don't.  This case only arises when we have an
     * EXCEPT or INTERSECT as child, else there won't be resjunk anyway.
     */
    result = list_make1(recurse_set_operations(setOp, root,
                                               top_union->colTypes,
                                               top_union->colCollations,
                                               false, -1,
                                               refnames_tlist,
                                               &child_tlist,
                                               NULL));
    *tlist_list = list_make1(child_tlist);
    return result;
}

/*
 * Add nodes to the given path tree to unique-ify the result of a UNION.
 */
static Path *
make_union_unique(SetOperationStmt *op, Path *path, List *tlist,
                  PlannerInfo *root)
{
    RelOptInfo *result_rel = fetch_upper_rel(root, UPPERREL_SETOP, NULL);
    List       *groupList;
    double        dNumGroups;

    /* Identify the grouping semantics */
    groupList = generate_setop_grouplist(op, tlist);

    /* punt if nothing to group on (can this happen?) */
    if (groupList == NIL)
        return path;

    /*
     * XXX for the moment, take the number of distinct groups as equal to the
     * total input size, ie, the worst case.  This is too conservative, but we
     * don't want to risk having the hashtable overrun memory; also, it's not
     * clear how to get a decent estimate of the true size.  One should note
     * as well the propensity of novices to write UNION rather than UNION ALL
     * even when they don't expect any duplicates...
     */
    dNumGroups = path->rows;

    /* Decide whether to hash or sort */
    if (choose_hashed_setop(root, groupList, path,
                            dNumGroups, dNumGroups,
                            "UNION"))
    {
        /* Hashed aggregate plan --- no sort needed */
        path = (Path *) create_agg_path(root,
                                        result_rel,
                                        path,
                                        create_pathtarget(root, tlist),
                                        AGG_HASHED,
                                        AGGSPLIT_SIMPLE,
                                        groupList,
                                        NIL,
                                        NULL,
                                        dNumGroups);
    }
    else
    {
        /* Sort and Unique */
        path = (Path *) create_sort_path(root,
                                         result_rel,
                                         path,
                                         make_pathkeys_for_sortclauses(root,
                                                                       groupList,
                                                                       tlist),
                                         -1.0);
        /* We have to manually jam the right tlist into the path; ick */
        path->pathtarget = create_pathtarget(root, tlist);
        path = (Path *) create_upper_unique_path(root,
                                                 result_rel,
                                                 path,
                                                 list_length(path->pathkeys),
                                                 dNumGroups);
    }

    return path;
}

/*
 * choose_hashed_setop - should we use hashing for a set operation?
 */
static bool
choose_hashed_setop(PlannerInfo *root, List *groupClauses,
                    Path *input_path,
                    double dNumGroups, double dNumOutputRows,
                    const char *construct)
{// #lizard forgives
    int            numGroupCols = list_length(groupClauses);
    bool        can_sort;
    bool        can_hash;
    Size        hashentrysize;
    Path        hashed_p;
    Path        sorted_p;
    double        tuple_fraction;

    /* Check whether the operators support sorting or hashing */
    can_sort = grouping_is_sortable(groupClauses);
    can_hash = grouping_is_hashable(groupClauses);
    if (can_hash && can_sort)
    {
        /* we have a meaningful choice to make, continue ... */
    }
    else if (can_hash)
        return true;
    else if (can_sort)
        return false;
    else
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        /* translator: %s is UNION, INTERSECT, or EXCEPT */
                 errmsg("could not implement %s", construct),
                 errdetail("Some of the datatypes only support hashing, while others only support sorting.")));

    /* Prefer sorting when enable_hashagg is off */
    if (!enable_hashagg)
        return false;

    /*
     * Don't do it if it doesn't look like the hashtable will fit into
     * work_mem.
     */
    hashentrysize = MAXALIGN(input_path->pathtarget->width) + MAXALIGN(SizeofMinimalTupleHeader);

    if (hashentrysize * dNumGroups > work_mem * 1024L)
        return false;

    /*
     * See if the estimated cost is no more than doing it the other way.
     *
     * We need to consider input_plan + hashagg versus input_plan + sort +
     * group.  Note that the actual result plan might involve a SetOp or
     * Unique node, not Agg or Group, but the cost estimates for Agg and Group
     * should be close enough for our purposes here.
     *
     * These path variables are dummies that just hold cost fields; we don't
     * make actual Paths for these steps.
     */
    cost_agg(&hashed_p, root, AGG_HASHED, NULL,
             numGroupCols, dNumGroups,
             input_path->startup_cost, input_path->total_cost,
             input_path->rows);

    /*
     * Now for the sorted case.  Note that the input is *always* unsorted,
     * since it was made by appending unrelated sub-relations together.
     */
    sorted_p.startup_cost = input_path->startup_cost;
    sorted_p.total_cost = input_path->total_cost;
    /* XXX cost_sort doesn't actually look at pathkeys, so just pass NIL */
    cost_sort(&sorted_p, root, NIL, sorted_p.total_cost,
              input_path->rows, input_path->pathtarget->width,
              0.0, work_mem, -1.0);
    cost_group(&sorted_p, root, numGroupCols, dNumGroups,
               sorted_p.startup_cost, sorted_p.total_cost,
               input_path->rows);

    /*
     * Now make the decision using the top-level tuple fraction.  First we
     * have to convert an absolute count (LIMIT) into fractional form.
     */
    tuple_fraction = root->tuple_fraction;
    if (tuple_fraction >= 1.0)
        tuple_fraction /= dNumOutputRows;

    if (compare_fractional_path_costs(&hashed_p, &sorted_p,
                                      tuple_fraction) < 0)
    {
        /* Hashed is cheaper, so use it */
        return true;
    }
    return false;
}

/*
 * Generate targetlist for a set-operation plan node
 *
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * flag: -1 if no flag column needed, 0 or 1 to create a const flag column
 * varno: varno to use in generated Vars
 * hack_constants: true to copy up constants (see comments in code)
 * input_tlist: targetlist of this node's input node
 * refnames_tlist: targetlist to take column names from
 */
static List *
generate_setop_tlist(List *colTypes, List *colCollations,
                     int flag,
                     Index varno,
                     bool hack_constants,
                     List *input_tlist,
                     List *refnames_tlist)
{
    List       *tlist = NIL;
    int            resno = 1;
    ListCell   *ctlc,
               *cclc,
               *itlc,
               *rtlc;
    TargetEntry *tle;
    Node       *expr;

    /* there's no forfour() so we must chase one list manually */
    rtlc = list_head(refnames_tlist);
    forthree(ctlc, colTypes, cclc, colCollations, itlc, input_tlist)
    {
        Oid            colType = lfirst_oid(ctlc);
        Oid            colColl = lfirst_oid(cclc);
        TargetEntry *inputtle = (TargetEntry *) lfirst(itlc);
        TargetEntry *reftle = (TargetEntry *) lfirst(rtlc);

        rtlc = lnext(rtlc);

        Assert(inputtle->resno == resno);
        Assert(reftle->resno == resno);
        Assert(!inputtle->resjunk);
        Assert(!reftle->resjunk);

        /*
         * Generate columns referencing input columns and having appropriate
         * data types and column names.  Insert datatype coercions where
         * necessary.
         *
         * HACK: constants in the input's targetlist are copied up as-is
         * rather than being referenced as subquery outputs.  This is mainly
         * to ensure that when we try to coerce them to the output column's
         * datatype, the right things happen for UNKNOWN constants.  But do
         * this only at the first level of subquery-scan plans; we don't want
         * phony constants appearing in the output tlists of upper-level
         * nodes!
         */
        if (hack_constants && inputtle->expr && IsA(inputtle->expr, Const))
            expr = (Node *) inputtle->expr;
        else
            expr = (Node *) makeVar(varno,
                                    inputtle->resno,
                                    exprType((Node *) inputtle->expr),
                                    exprTypmod((Node *) inputtle->expr),
                                    exprCollation((Node *) inputtle->expr),
                                    0);

        if (exprType(expr) != colType)
        {
            /*
             * Note: it's not really cool to be applying coerce_to_common_type
             * here; one notable point is that assign_expr_collations never
             * gets run on any generated nodes.  For the moment that's not a
             * problem because we force the correct exposed collation below.
             * It would likely be best to make the parser generate the correct
             * output tlist for every set-op to begin with, though.
             */
            expr = coerce_to_common_type(NULL,    /* no UNKNOWNs here */
                                         expr,
                                         colType,
                                         "UNION/INTERSECT/EXCEPT");
        }

        /*
         * Ensure the tlist entry's exposed collation matches the set-op. This
         * is necessary because plan_set_operations() reports the result
         * ordering as a list of SortGroupClauses, which don't carry collation
         * themselves but just refer to tlist entries.  If we don't show the
         * right collation then planner.c might do the wrong thing in
         * higher-level queries.
         *
         * Note we use RelabelType, not CollateExpr, since this expression
         * will reach the executor without any further processing.
         */
        if (exprCollation(expr) != colColl)
        {
            expr = (Node *) makeRelabelType((Expr *) expr,
                                            exprType(expr),
                                            exprTypmod(expr),
                                            colColl,
                                            COERCE_IMPLICIT_CAST);
        }

        tle = makeTargetEntry((Expr *) expr,
                              (AttrNumber) resno++,
                              pstrdup(reftle->resname),
                              false);

        /*
         * By convention, all non-resjunk columns in a setop tree have
         * ressortgroupref equal to their resno.  In some cases the ref isn't
         * needed, but this is a cleaner way than modifying the tlist later.
         */
        tle->ressortgroupref = tle->resno;

        tlist = lappend(tlist, tle);
    }

    if (flag >= 0)
    {
        /* Add a resjunk flag column */
        /* flag value is the given constant */
        expr = (Node *) makeConst(INT4OID,
                                  -1,
                                  InvalidOid,
                                  sizeof(int32),
                                  Int32GetDatum(flag),
                                  false,
                                  true);
        tle = makeTargetEntry((Expr *) expr,
                              (AttrNumber) resno++,
                              pstrdup("flag"),
                              true);
        tlist = lappend(tlist, tle);
    }

    return tlist;
}

/*
 * Generate targetlist for a set-operation Append node
 *
 * colTypes: OID list of set-op's result column datatypes
 * colCollations: OID list of set-op's result column collations
 * flag: true to create a flag column copied up from subplans
 * input_tlists: list of tlists for sub-plans of the Append
 * refnames_tlist: targetlist to take column names from
 *
 * The entries in the Append's targetlist should always be simple Vars;
 * we just have to make sure they have the right datatypes/typmods/collations.
 * The Vars are always generated with varno 0.
 *
 * XXX a problem with the varno-zero approach is that set_pathtarget_cost_width
 * cannot figure out a realistic width for the tlist we make here.  But we
 * ought to refactor this code to produce a PathTarget directly, anyway.
 */
static List *
generate_append_tlist(List *colTypes, List *colCollations,
                      bool flag,
                      List *input_tlists,
                      List *refnames_tlist)
{
    List       *tlist = NIL;
    int            resno = 1;
    ListCell   *curColType;
    ListCell   *curColCollation;
    ListCell   *ref_tl_item;
    int            colindex;
    TargetEntry *tle;
    Node       *expr;
    ListCell   *tlistl;
    int32       *colTypmods;

    /*
     * First extract typmods to use.
     *
     * If the inputs all agree on type and typmod of a particular column, use
     * that typmod; else use -1.
     */
    colTypmods = (int32 *) palloc(list_length(colTypes) * sizeof(int32));

    foreach(tlistl, input_tlists)
    {
        List       *subtlist = (List *) lfirst(tlistl);
        ListCell   *subtlistl;

        curColType = list_head(colTypes);
        colindex = 0;
        foreach(subtlistl, subtlist)
        {
            TargetEntry *subtle = (TargetEntry *) lfirst(subtlistl);

            if (subtle->resjunk)
                continue;
            Assert(curColType != NULL);
            if (exprType((Node *) subtle->expr) == lfirst_oid(curColType))
            {
                /* If first subplan, copy the typmod; else compare */
                int32        subtypmod = exprTypmod((Node *) subtle->expr);

                if (tlistl == list_head(input_tlists))
                    colTypmods[colindex] = subtypmod;
                else if (subtypmod != colTypmods[colindex])
                    colTypmods[colindex] = -1;
            }
            else
            {
                /* types disagree, so force typmod to -1 */
                colTypmods[colindex] = -1;
            }
            curColType = lnext(curColType);
            colindex++;
        }
        Assert(curColType == NULL);
    }

    /*
     * Now we can build the tlist for the Append.
     */
    colindex = 0;
    forthree(curColType, colTypes, curColCollation, colCollations,
             ref_tl_item, refnames_tlist)
    {
        Oid            colType = lfirst_oid(curColType);
        int32        colTypmod = colTypmods[colindex++];
        Oid            colColl = lfirst_oid(curColCollation);
        TargetEntry *reftle = (TargetEntry *) lfirst(ref_tl_item);

        Assert(reftle->resno == resno);
        Assert(!reftle->resjunk);
        expr = (Node *) makeVar(0,
                                resno,
                                colType,
                                colTypmod,
                                colColl,
                                0);
        tle = makeTargetEntry((Expr *) expr,
                              (AttrNumber) resno++,
                              pstrdup(reftle->resname),
                              false);

        /*
         * By convention, all non-resjunk columns in a setop tree have
         * ressortgroupref equal to their resno.  In some cases the ref isn't
         * needed, but this is a cleaner way than modifying the tlist later.
         */
        tle->ressortgroupref = tle->resno;

        tlist = lappend(tlist, tle);
    }

    if (flag)
    {
        /* Add a resjunk flag column */
        /* flag value is shown as copied up from subplan */
        expr = (Node *) makeVar(0,
                                resno,
                                INT4OID,
                                -1,
                                InvalidOid,
                                0);
        tle = makeTargetEntry((Expr *) expr,
                              (AttrNumber) resno++,
                              pstrdup("flag"),
                              true);
        tlist = lappend(tlist, tle);
    }

    pfree(colTypmods);

    return tlist;
}

/*
 * generate_setop_grouplist
 *        Build a SortGroupClause list defining the sort/grouping properties
 *        of the setop's output columns.
 *
 * Parse analysis already determined the properties and built a suitable
 * list, except that the entries do not have sortgrouprefs set because
 * the parser output representation doesn't include a tlist for each
 * setop.  So what we need to do here is copy that list and install
 * proper sortgrouprefs into it (copying those from the targetlist).
 */
static List *
generate_setop_grouplist(SetOperationStmt *op, List *targetlist)
{
    List       *grouplist = copyObject(op->groupClauses);
    ListCell   *lg;
    ListCell   *lt;

    lg = list_head(grouplist);
    foreach(lt, targetlist)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(lt);
        SortGroupClause *sgc;

        if (tle->resjunk)
        {
            /* resjunk columns should not have sortgrouprefs */
            Assert(tle->ressortgroupref == 0);
            continue;            /* ignore resjunk columns */
        }

        /* non-resjunk columns should have sortgroupref = resno */
        Assert(tle->ressortgroupref == tle->resno);

        /* non-resjunk columns should have grouping clauses */
        Assert(lg != NULL);
        sgc = (SortGroupClause *) lfirst(lg);
        lg = lnext(lg);
        Assert(sgc->tleSortGroupRef == 0);

        sgc->tleSortGroupRef = tle->ressortgroupref;
    }
    Assert(lg == NULL);
    return grouplist;
}


/*
 * expand_inherited_tables
 *        Expand each rangetable entry that represents an inheritance set
 *        into an "append relation".  At the conclusion of this process,
 *        the "inh" flag is set in all and only those RTEs that are append
 *        relation parents.
 */
void
expand_inherited_tables(PlannerInfo *root)
{
    Index        nrtes;
    Index        rti;
    ListCell   *rl;

    /*
	 * expand_inherited_rtentry may add RTEs to parse->rtable. The function is
	 * expected to recursively handle any RTEs that it creates with inh=true.
	 * So just scan as far as the original end of the rtable list.
     */
    nrtes = list_length(root->parse->rtable);
    rl = list_head(root->parse->rtable);
    for (rti = 1; rti <= nrtes; rti++)
    {
        RangeTblEntry *rte = (RangeTblEntry *) lfirst(rl);

        expand_inherited_rtentry(root, rte, rti);
        rl = lnext(rl);
    }
}

/*
 * expand_inherited_rtentry
 *        Check whether a rangetable entry represents an inheritance set.
 *        If so, add entries for all the child tables to the query's
 *        rangetable, and build AppendRelInfo nodes for all the child tables
 *        and add them to root->append_rel_list.  If not, clear the entry's
 *        "inh" flag to prevent later code from looking for AppendRelInfos.
 *
 * Note that the original RTE is considered to represent the whole
 * inheritance set.  The first of the generated RTEs is an RTE for the same
 * table, but with inh = false, to represent the parent table in its role
 * as a simple member of the inheritance set.
 *
* A childless table is never considered to be an inheritance set. For
* regular inheritance, a parent RTE must always have at least two associated
* AppendRelInfos: one corresponding to the parent table as a simple member of
* inheritance set and one or more corresponding to the actual children.
* Since a partitioned table is not scanned, it might have only one associated
* AppendRelInfo.
 */
static void
expand_inherited_rtentry(PlannerInfo *root, RangeTblEntry *rte, Index rti)
{// #lizard forgives
    Query       *parse = root->parse;
    Oid            parentOID;
    PlanRowMark *oldrc;
    Relation    oldrelation;
    LOCKMODE    lockmode;
    List       *inhOIDs;
    ListCell   *l;

    /* Does RT entry allow inheritance? */
    if (!rte->inh)
        return;
    /* Ignore any already-expanded UNION ALL nodes */
    if (rte->rtekind != RTE_RELATION)
    {
        Assert(rte->rtekind == RTE_SUBQUERY);
        return;
    }
    /* Fast path for common case of childless table */
    parentOID = rte->relid;
    if (!has_subclass(parentOID))
    {
        /* Clear flag before returning */
        rte->inh = false;
        return;
    }

    /*
     * The rewriter should already have obtained an appropriate lock on each
     * relation named in the query.  However, for each child relation we add
     * to the query, we must obtain an appropriate lock, because this will be
     * the first use of those relations in the parse/rewrite/plan pipeline.
     *
     * If the parent relation is the query's result relation, then we need
     * RowExclusiveLock.  Otherwise, if it's accessed FOR UPDATE/SHARE, we
     * need RowShareLock; otherwise AccessShareLock.  We can't just grab
     * AccessShareLock because then the executor would be trying to upgrade
     * the lock, leading to possible deadlocks.  (This code should match the
     * parser and rewriter.)
     */
    oldrc = get_plan_rowmark(root->rowMarks, rti);
    if (rti == parse->resultRelation)
        lockmode = RowExclusiveLock;
    else if (oldrc && RowMarkRequiresRowShareLock(oldrc->markType))
        lockmode = RowShareLock;
    else
        lockmode = AccessShareLock;

    /* Scan for all members of inheritance set, acquire needed locks */
    inhOIDs = find_all_inheritors(parentOID, lockmode, NULL);

    /*
     * Check that there's at least one descendant, else treat as no-child
     * case.  This could happen despite above has_subclass() check, if table
     * once had a child but no longer does.
     */
    if (list_length(inhOIDs) < 2)
    {
        /* Clear flag before returning */
        rte->inh = false;
        return;
    }

    /*
     * If parent relation is selected FOR UPDATE/SHARE, we need to mark its
     * PlanRowMark as isParent = true, and generate a new PlanRowMark for each
     * child.
     */
    if (oldrc)
        oldrc->isParent = true;

    /*
     * Must open the parent relation to examine its tupdesc.  We need not lock
     * it; we assume the rewriter already did.
     */
    oldrelation = heap_open(parentOID, NoLock);

    /* Scan the inheritance set and expand it */
   if (RelationGetPartitionDesc(oldrelation) != NULL)
   {
		Assert(rte->relkind == RELKIND_PARTITIONED_TABLE);

       /*
        * If this table has partitions, recursively expand them in the order
		 * in which they appear in the PartitionDesc.  While at it, also
		 * extract the partition key columns of all the partitioned tables.
        */
       expand_partitioned_rtentry(root, rte, rti, oldrelation, oldrc,
								   lockmode, &root->append_rel_list);
   }
   else
   {
		List	   *appinfos = NIL;
		RangeTblEntry *childrte;
		Index		childRTindex;

       /*
        * This table has no partitions.  Expand any plain inheritance
        * children in the order the OIDs were returned by
        * find_all_inheritors.
        */
    foreach(l, inhOIDs)
    {
        Oid            childOID = lfirst_oid(l);
        Relation    newrelation;

        /* Open rel if needed; we already have required locks */
        if (childOID != parentOID)
            newrelation = heap_open(childOID, NoLock);
        else
            newrelation = oldrelation;

        /*
         * It is possible that the parent table has children that are temp
         * tables of other backends.  We cannot safely access such tables
            * (because of buffering issues), and the best thing to do seems
            * to be to silently ignore them.
         */
        if (childOID != parentOID && RELATION_IS_OTHER_TEMP(newrelation))
        {
            heap_close(newrelation, lockmode);
            continue;
        }

           expand_single_inheritance_child(root, rte, rti, oldrelation, oldrc,
                                           newrelation,
											&appinfos, &childrte,
											&childRTindex);

           /* Close child relations, but keep locks */
           if (childOID != parentOID)
               heap_close(newrelation, NoLock);
       }

        /*
		 * If all the children were temp tables, pretend it's a
		 * non-inheritance situation; we don't need Append node in that case.
		 * The duplicate RTE we added for the parent table is harmless, so we
		 * don't bother to get rid of it; ditto for the useless PlanRowMark
		 * node.
         */
		if (list_length(appinfos) < 2)
       rte->inh = false;
		else
			root->append_rel_list = list_concat(root->append_rel_list,
												appinfos);

   }

	heap_close(oldrelation, NoLock);
}

/*
 * expand_partitioned_rtentry
 *		Recursively expand an RTE for a partitioned table.
 *
 * Note that RelationGetPartitionDispatchInfo will expand partitions in the
 * same order as this code.
 */
static void
expand_partitioned_rtentry(PlannerInfo *root, RangeTblEntry *parentrte,
						   Index parentRTindex, Relation parentrel,
						   PlanRowMark *top_parentrc, LOCKMODE lockmode,
						   List **appinfos)
{
	int			i;
	RangeTblEntry *childrte;
	Index		childRTindex;
	bool		has_child = false;
	PartitionDesc partdesc = RelationGetPartitionDesc(parentrel);

	check_stack_depth();

	/* A partitioned table should always have a partition descriptor. */
	Assert(partdesc);

	Assert(parentrte->inh);

	/*
	 * Note down whether any partition key cols are being updated. Though it's
	 * the root partitioned table's updatedCols we are interested in, we
	 * instead use parentrte to get the updatedCols. This is convenient because
	 * parentrte already has the root partrel's updatedCols translated to match
	 * the attribute ordering of parentrel.
	 */
	if (!root->partColsUpdated)
		root->partColsUpdated =
			has_partition_attrs(parentrel, parentrte->updatedCols, NULL);

	/* First expand the partitioned table itself. */
	expand_single_inheritance_child(root, parentrte, parentRTindex, parentrel,
									top_parentrc, parentrel,
									appinfos, &childrte, &childRTindex);

	for (i = 0; i < partdesc->nparts; i++)
	{
		Oid			childOID = partdesc->oids[i];
		Relation	childrel;

		/* Open rel; we already have required locks */
		childrel = heap_open(childOID, NoLock);

		/* As in expand_inherited_rtentry, skip non-local temp tables */
		if (RELATION_IS_OTHER_TEMP(childrel))
		{
			heap_close(childrel, lockmode);
			continue;
		}

		/* We have a real partition. */
		has_child = true;

		expand_single_inheritance_child(root, parentrte, parentRTindex,
										parentrel, top_parentrc, childrel,
										appinfos, &childrte, &childRTindex);

		/* If this child is itself partitioned, recurse */
		if (childrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			expand_partitioned_rtentry(root, childrte, childRTindex,
									   childrel, top_parentrc, lockmode,
									   appinfos);

		/* Close child relation, but keep locks */
		heap_close(childrel, NoLock);
	}

	/*
	 * If the partitioned table has no partitions or all the partitions are
	 * temporary tables from other backends, treat this as non-inheritance
	 * case.
	 */
	if (!has_child)
		parentrte->inh = false;
}

/*
 * expand_single_inheritance_child
 *		Expand a single inheritance child, if needed.
 *
 * If this is a temp table of another backend, we'll return without doing
 * anything at all.  Otherwise, build a RangeTblEntry and an AppendRelInfo, if
 * appropriate, plus maybe a PlanRowMark.
 *
 * We now expand the partition hierarchy level by level, creating a
 * corresponding hierarchy of AppendRelInfos and RelOptInfos, where each
 * partitioned descendant acts as a parent of its immediate partitions.
 * (This is a difference from what older versions of PostgreSQL did and what
 * is still done in the case of table inheritance for unpartitioned tables,
 * where the hierarchy is flattened during RTE expansion.)
 *
 * PlanRowMarks still carry the top-parent's RTI, and the top-parent's
 * allMarkTypes field still accumulates values from all descendents.
 *
 * "parentrte" and "parentRTindex" are immediate parent's RTE and
 * RTI. "top_parentrc" is top parent's PlanRowMark.
 *
 * The child RangeTblEntry and its RTI are returned in "childrte_p" and
 * "childRTindex_p" resp.
 */
static void
expand_single_inheritance_child(PlannerInfo *root, RangeTblEntry *parentrte,
								Index parentRTindex, Relation parentrel,
								PlanRowMark *top_parentrc, Relation childrel,
								List **appinfos, RangeTblEntry **childrte_p,
								Index *childRTindex_p)
{
	Query	   *parse = root->parse;
	Oid			parentOID = RelationGetRelid(parentrel);
	Oid			childOID = RelationGetRelid(childrel);
	RangeTblEntry *childrte;
	Index		childRTindex;
	AppendRelInfo *appinfo;

	/*
	 * Build an RTE for the child, and attach to query's rangetable list. We
	 * copy most fields of the parent's RTE, but replace relation OID and
	 * relkind, and set inh = false.  Also, set requiredPerms to zero since
	 * all required permissions checks are done on the original RTE. Likewise,
	 * set the child's securityQuals to empty, because we only want to apply
	 * the parent's RLS conditions regardless of what RLS properties
	 * individual children may have.  (This is an intentional choice to make
	 * inherited RLS work like regular permissions checks.) The parent
	 * securityQuals will be propagated to children along with other base
	 * restriction clauses, so we don't need to do it here.
	 */
	childrte = copyObject(parentrte);
	*childrte_p = childrte;
        childrte->relid = childOID;
	childrte->relkind = childrel->rd_rel->relkind;
	/* A partitioned child will need to be expanded further. */
	if (childOID != parentOID &&
		childrte->relkind == RELKIND_PARTITIONED_TABLE)
		childrte->inh = true;
	else
        childrte->inh = false;
        childrte->requiredPerms = 0;
        childrte->securityQuals = NIL;
        parse->rtable = lappend(parse->rtable, childrte);
        childRTindex = list_length(parse->rtable);
	*childRTindex_p = childRTindex;

        /*
	 * We need an AppendRelInfo if paths will be built for the child RTE. If
	 * childrte->inh is true, then we'll always need to generate append paths
	 * for it.  If childrte->inh is false, we must scan it if it's not a
	 * partitioned table; but if it is a partitioned table, then it never has
	 * any data of its own and need not be scanned.
         */
	if (childrte->relkind != RELKIND_PARTITIONED_TABLE || childrte->inh)
        {
            appinfo = makeNode(AppendRelInfo);
		appinfo->parent_relid = parentRTindex;
            appinfo->child_relid = childRTindex;
		appinfo->parent_reltype = parentrel->rd_rel->reltype;
		appinfo->child_reltype = childrel->rd_rel->reltype;
		make_inh_translation_list(parentrel, childrel, childRTindex,
                                      &appinfo->translated_vars);
            appinfo->parent_reloid = parentOID;
		*appinfos = lappend(*appinfos, appinfo);

            /*
		 * Translate the column permissions bitmaps to the child's attnums (we
		 * have to build the translated_vars list before we can do this). But
		 * if this is the parent table, leave copyObject's result alone.
             *
             * Note: we need to do this even though the executor won't run any
		 * permissions checks on the child RTE.  The insertedCols/updatedCols
		 * bitmaps may be examined for trigger-firing purposes.
             */
            if (childOID != parentOID)
            {
			childrte->selectedCols = translate_col_privs(parentrte->selectedCols,
                                                             appinfo->translated_vars);
			childrte->insertedCols = translate_col_privs(parentrte->insertedCols,
                                                             appinfo->translated_vars);
			childrte->updatedCols = translate_col_privs(parentrte->updatedCols,
                                                            appinfo->translated_vars);
            }
        }

        /*
         * Build a PlanRowMark if parent is marked FOR UPDATE/SHARE.
         */
	if (top_parentrc)
        {
		PlanRowMark *childrc = makeNode(PlanRowMark);

		childrc->rti = childRTindex;
		childrc->prti = top_parentrc->rti;
		childrc->rowmarkId = top_parentrc->rowmarkId;
            /* Reselect rowmark type, because relkind might not match parent */
		childrc->markType = select_rowmark_type(childrte,
												top_parentrc->strength);
		childrc->allMarkTypes = (1 << childrc->markType);
		childrc->strength = top_parentrc->strength;
		childrc->waitPolicy = top_parentrc->waitPolicy;

            /*
		 * We mark RowMarks for partitioned child tables as parent RowMarks so
		 * that the executor ignores them (except their existence means that
		 * the child tables be locked using appropriate mode).
             */
		childrc->isParent = (childrte->relkind == RELKIND_PARTITIONED_TABLE);

		/* Include child's rowmark type in top parent's allMarkTypes */
		top_parentrc->allMarkTypes |= childrc->allMarkTypes;

		root->rowMarks = lappend(root->rowMarks, childrc);
    }
}

/*
 * make_inh_translation_list
 *      Build the list of translations from parent Vars to child Vars for
 *      an inheritance child.
 *
 * For paranoia's sake, we match type/collation as well as attribute name.
 */
static void
make_inh_translation_list(Relation oldrelation, Relation newrelation,
                          Index newvarno,
                          List **translated_vars)
{// #lizard forgives
    List       *vars = NIL;
    TupleDesc    old_tupdesc = RelationGetDescr(oldrelation);
    TupleDesc    new_tupdesc = RelationGetDescr(newrelation);
	Oid			new_relid = RelationGetRelid(newrelation);
    int            oldnatts = old_tupdesc->natts;
    int            newnatts = new_tupdesc->natts;
    int            old_attno;
	int			new_attno = 0;

    for (old_attno = 0; old_attno < oldnatts; old_attno++)
    {
        Form_pg_attribute att;
        char       *attname;
        Oid            atttypid;
        int32        atttypmod;
        Oid            attcollation;

        att = old_tupdesc->attrs[old_attno];
        if (att->attisdropped)
        {
            /* Just put NULL into this list entry */
            vars = lappend(vars, NULL);
            continue;
        }
        attname = NameStr(att->attname);
        atttypid = att->atttypid;
        atttypmod = att->atttypmod;
        attcollation = att->attcollation;

        /*
         * When we are generating the "translation list" for the parent table
         * of an inheritance set, no need to search for matches.
         */
        if (oldrelation == newrelation)
        {
            vars = lappend(vars, makeVar(newvarno,
                                         (AttrNumber) (old_attno + 1),
                                         atttypid,
                                         atttypmod,
                                         attcollation,
                                         0));
            continue;
        }

        /*
         * Otherwise we have to search for the matching column by name.
         * There's no guarantee it'll have the same column position, because
         * of cases like ALTER TABLE ADD COLUMN and multiple inheritance.
         * However, in simple cases, the relative order of columns is mostly
         * the same in both relations, so try the column of newrelation that
         * follows immediately after the one that we just found, and if that
         * fails, let syscache handle it.
		 */
        if (new_attno >= newnatts ||
            (att = TupleDescAttr(new_tupdesc, new_attno))->attisdropped ||
            strcmp(attname, NameStr(att->attname)) != 0)
            {
            HeapTuple   newtup;

            newtup = SearchSysCacheAttName(new_relid, attname);
            if (!newtup)
                elog(ERROR, "could not find inherited attribute \"%s\" of relation \"%s\"",
                     attname, RelationGetRelationName(newrelation));
            new_attno = ((Form_pg_attribute) GETSTRUCT(newtup))->attnum - 1;
            ReleaseSysCache(newtup);

            att = TupleDescAttr(new_tupdesc, new_attno);
        }

        /* Found it, check type and collation match */
        if (atttypid != att->atttypid || atttypmod != att->atttypmod)
            elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's type",
                 attname, RelationGetRelationName(newrelation));
        if (attcollation != att->attcollation)
            elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's collation",
                 attname, RelationGetRelationName(newrelation));

        vars = lappend(vars, makeVar(newvarno,
                                     (AttrNumber) (new_attno + 1),
                                     atttypid,
                                     atttypmod,
                                     attcollation,
                                     0));
		new_attno++;
    }

    *translated_vars = vars;
}

/*
 * translate_col_privs
 *      Translate a bitmapset representing per-column privileges from the
 *      parent rel's attribute numbering to the child's.
 *
 * The only surprise here is that we don't translate a parent whole-row
 * reference into a child whole-row reference.  That would mean requiring
 * permissions on all child columns, which is overly strict, since the
 * query is really only going to reference the inherited columns.  Instead
 * we set the per-column bits for all inherited columns.
 */
static Bitmapset *
translate_col_privs(const Bitmapset *parent_privs,
                    List *translated_vars)
{
    Bitmapset  *child_privs = NULL;
    bool        whole_row;
    int            attno;
    ListCell   *lc;

    /* System attributes have the same numbers in all tables */
    for (attno = FirstLowInvalidHeapAttributeNumber + 1; attno < 0; attno++)
    {
        if (bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
                          parent_privs))
            child_privs = bms_add_member(child_privs,
                                         attno - FirstLowInvalidHeapAttributeNumber);
    }

    /* Check if parent has whole-row reference */
    whole_row = bms_is_member(InvalidAttrNumber - FirstLowInvalidHeapAttributeNumber,
                              parent_privs);

    /* And now translate the regular user attributes, using the vars list */
    attno = InvalidAttrNumber;
    foreach(lc, translated_vars)
    {
        Var           *var = lfirst_node(Var, lc);

        attno++;
        if (var == NULL)        /* ignore dropped columns */
            continue;
        if (whole_row ||
            bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
                          parent_privs))
            child_privs = bms_add_member(child_privs,
                                         var->varattno - FirstLowInvalidHeapAttributeNumber);
    }

    return child_privs;
}

/*
 * adjust_appendrel_attrs
 *        Copy the specified query or expression and translate Vars referring to a
 *        parent rel to refer to the corresponding child rel instead.  We also
 *        update rtindexes appearing outside Vars, such as resultRelation and
 *        jointree relids.
 *
 * Note: this is only applied after conversion of sublinks to subplans,
 * so we don't need to cope with recursion into sub-queries.
 *
 * Note: this is not hugely different from what pullup_replace_vars() does;
 * maybe we should try to fold the two routines together.
 */
Node *
adjust_appendrel_attrs(PlannerInfo *root, Node *node, int nappinfos,
                                           AppendRelInfo **appinfos)
{
        Node       *result;
        adjust_appendrel_attrs_context context;

        context.root = root;
        context.nappinfos = nappinfos;
        context.appinfos = appinfos;

        /* If there's nothing to adjust, don't call this function. */
        Assert(nappinfos >= 1 && appinfos != NULL);

        /*
         * Must be prepared to start with a Query or a bare expression tree.
         */
        if (node && IsA(node, Query))
        {
                Query      *newnode;
                int                     cnt;

                newnode = query_tree_mutator((Query *) node,
                                                                         adjust_appendrel_attrs_mutator,
                                                                         (void *) &context,
                                                                         QTW_IGNORE_RC_SUBQUERIES);
                for (cnt = 0; cnt < nappinfos; cnt++)
                {
                        AppendRelInfo *appinfo = appinfos[cnt];

                        if (newnode->resultRelation == appinfo->parent_relid)
                        {
                                newnode->resultRelation = appinfo->child_relid;
                                /* Fix tlist resnos too, if it's inherited UPDATE */
                                if (newnode->commandType == CMD_UPDATE)
                                        newnode->targetList =
                                                adjust_inherited_tlist(newnode->targetList,
                                                                                           appinfo);
                                break;
                        }
                }

                result = (Node *) newnode;
        }
        else
                result = adjust_appendrel_attrs_mutator(node, &context);

        return result;
}

static Node *
adjust_appendrel_attrs_mutator(Node *node,
                               adjust_appendrel_attrs_context *context)
{
	AppendRelInfo **appinfos = context->appinfos;
    int         nappinfos = context->nappinfos;
    int         cnt;

    if (node == NULL)
        return NULL;
    if (IsA(node, Var))
    {
        Var           *var = (Var *) copyObject(node);
		AppendRelInfo *appinfo = NULL;

        for (cnt = 0; cnt < nappinfos; cnt++)
        {
            if (var->varno == appinfos[cnt]->parent_relid)
            {
                appinfo = appinfos[cnt];
                break;
            }
        }

        if (var->varlevelsup == 0 && appinfo)
        {
            var->varno = appinfo->child_relid;
            var->varnoold = appinfo->child_relid;
            if (var->varattno > 0)
            {
                Node       *newnode;

                if (var->varattno > list_length(appinfo->translated_vars))
                    elog(ERROR, "attribute %d of relation \"%s\" does not exist",
                         var->varattno, get_rel_name(appinfo->parent_reloid));
                newnode = copyObject(list_nth(appinfo->translated_vars,
                                              var->varattno - 1));
                if (newnode == NULL)
                    elog(ERROR, "attribute %d of relation \"%s\" does not exist",
                         var->varattno, get_rel_name(appinfo->parent_reloid));
                return newnode;
            }
            else if (var->varattno == 0)
            {
                /*
                 * Whole-row Var: if we are dealing with named rowtypes, we
                 * can use a whole-row Var for the child table plus a coercion
                 * step to convert the tuple layout to the parent's rowtype.
                 * Otherwise we have to generate a RowExpr.
                 */
                if (OidIsValid(appinfo->child_reltype))
                {
                    Assert(var->vartype == appinfo->parent_reltype);
                    if (appinfo->parent_reltype != appinfo->child_reltype)
                    {
                        ConvertRowtypeExpr *r = makeNode(ConvertRowtypeExpr);

                        r->arg = (Expr *) var;
                        r->resulttype = appinfo->parent_reltype;
                        r->convertformat = COERCE_IMPLICIT_CAST;
                        r->location = -1;
                        /* Make sure the Var node has the right type ID, too */
                        var->vartype = appinfo->child_reltype;
                        return (Node *) r;
                    }
                }
                else
                {
                    /*
                     * Build a RowExpr containing the translated variables.
                     *
                     * In practice var->vartype will always be RECORDOID here,
                     * so we need to come up with some suitable column names.
                     * We use the parent RTE's column names.
                     *
                     * Note: we can't get here for inheritance cases, so there
                     * is no need to worry that translated_vars might contain
                     * some dummy NULLs.
                     */
                    RowExpr    *rowexpr;
                    List       *fields;
                    RangeTblEntry *rte;

                    rte = rt_fetch(appinfo->parent_relid,
                                   context->root->parse->rtable);
                    fields = copyObject(appinfo->translated_vars);
                    rowexpr = makeNode(RowExpr);
                    rowexpr->args = fields;
                    rowexpr->row_typeid = var->vartype;
                    rowexpr->row_format = COERCE_IMPLICIT_CAST;
                    rowexpr->colnames = copyObject(rte->eref->colnames);
                    rowexpr->location = -1;

                    return (Node *) rowexpr;
                }
            }
            /* system attributes don't need any other translation */
        }
        return (Node *) var;
    }
    if (IsA(node, CurrentOfExpr))
    {
        CurrentOfExpr *cexpr = (CurrentOfExpr *) copyObject(node);

		for (cnt = 0; cnt < nappinfos; cnt++)
        {
            AppendRelInfo *appinfo = appinfos[cnt];
 
        if (cexpr->cvarno == appinfo->parent_relid)
            {
            cexpr->cvarno = appinfo->child_relid;
                break;
            }
        }
        return (Node *) cexpr;
    }
    if (IsA(node, RangeTblRef))
    {
        RangeTblRef *rtr = (RangeTblRef *) copyObject(node);

		for (cnt = 0; cnt < nappinfos; cnt++)
        {
            AppendRelInfo *appinfo = appinfos[cnt];
 
        if (rtr->rtindex == appinfo->parent_relid)
            {
            rtr->rtindex = appinfo->child_relid;
                break;
            }
        }
        return (Node *) rtr;
    }
    if (IsA(node, JoinExpr))
    {
        /* Copy the JoinExpr node with correct mutation of subnodes */
        JoinExpr   *j;
		AppendRelInfo *appinfo;

        j = (JoinExpr *) expression_tree_mutator(node,
                                                 adjust_appendrel_attrs_mutator,
                                                 (void *) context);
        /* now fix JoinExpr's rtindex (probably never happens) */
		for (cnt = 0; cnt < nappinfos; cnt++)
        {
            appinfo = appinfos[cnt];
 
        if (j->rtindex == appinfo->parent_relid)
            {
            j->rtindex = appinfo->child_relid;
                break;
            }
        }
        return (Node *) j;
    }
    if (IsA(node, PlaceHolderVar))
    {
        /* Copy the PlaceHolderVar node with correct mutation of subnodes */
        PlaceHolderVar *phv;

        phv = (PlaceHolderVar *) expression_tree_mutator(node,
                                                         adjust_appendrel_attrs_mutator,
                                                         (void *) context);
        /* now fix PlaceHolderVar's relid sets */
        if (phv->phlevelsup == 0)
			phv->phrels = adjust_child_relids(phv->phrels, context->nappinfos,
                                             context->appinfos);
        return (Node *) phv;
    }
    /* Shouldn't need to handle planner auxiliary nodes here */
    Assert(!IsA(node, SpecialJoinInfo));
    Assert(!IsA(node, AppendRelInfo));
    Assert(!IsA(node, PlaceHolderInfo));
    Assert(!IsA(node, MinMaxAggInfo));

    /*
     * We have to process RestrictInfo nodes specially.  (Note: although
     * set_append_rel_pathlist will hide RestrictInfos in the parent's
     * baserestrictinfo list from us, it doesn't hide those in joininfo.)
     */
    if (IsA(node, RestrictInfo))
    {
        RestrictInfo *oldinfo = (RestrictInfo *) node;
        RestrictInfo *newinfo = makeNode(RestrictInfo);

        /* Copy all flat-copiable fields */
        memcpy(newinfo, oldinfo, sizeof(RestrictInfo));

        /* Recursively fix the clause itself */
        newinfo->clause = (Expr *)
            adjust_appendrel_attrs_mutator((Node *) oldinfo->clause, context);

        /* and the modified version, if an OR clause */
        newinfo->orclause = (Expr *)
            adjust_appendrel_attrs_mutator((Node *) oldinfo->orclause, context);

        /* adjust relid sets too */
        newinfo->clause_relids = adjust_child_relids(oldinfo->clause_relids,
                                                     context->nappinfos,
                                                     context->appinfos);
        newinfo->required_relids = adjust_child_relids(oldinfo->required_relids,
                                                       context->nappinfos,
                                                       context->appinfos);
        newinfo->outer_relids = adjust_child_relids(oldinfo->outer_relids,
                                                    context->nappinfos,
                                                    context->appinfos);
        newinfo->nullable_relids = adjust_child_relids(oldinfo->nullable_relids,
                                                       context->nappinfos,
                                                       context->appinfos);
        newinfo->left_relids = adjust_child_relids(oldinfo->left_relids,
                                                   context->nappinfos,
                                                   context->appinfos);
        newinfo->right_relids = adjust_child_relids(oldinfo->right_relids,
                                                    context->nappinfos,
                                                    context->appinfos);

        /*
         * Reset cached derivative fields, since these might need to have
         * different values when considering the child relation.  Note we
         * don't reset left_ec/right_ec: each child variable is implicitly
         * equivalent to its parent, so still a member of the same EC if any.
         */
        newinfo->eval_cost.startup = -1;
        newinfo->norm_selec = -1;
        newinfo->outer_selec = -1;
        newinfo->left_em = NULL;
        newinfo->right_em = NULL;
        newinfo->scansel_cache = NIL;
        newinfo->left_bucketsize = -1;
        newinfo->right_bucketsize = -1;

        return (Node *) newinfo;
    }

    /*
     * NOTE: we do not need to recurse into sublinks, because they should
     * already have been converted to subplans before we see them.
     */
    Assert(!IsA(node, SubLink));
    Assert(!IsA(node, Query));

    return expression_tree_mutator(node, adjust_appendrel_attrs_mutator,
                                   (void *) context);
}

/*
 * Substitute child relids for parent relids in a Relid set.  The array of
 * appinfos specifies the substitutions to be performed.
 */
static Relids
adjust_child_relids(Relids relids, int nappinfos, AppendRelInfo **appinfos)
{
        Bitmapset  *result = NULL;
        int                     cnt;

        for (cnt = 0; cnt < nappinfos; cnt++)
        {
                AppendRelInfo *appinfo = appinfos[cnt];

                /* Remove parent, add child */
                if (bms_is_member(appinfo->parent_relid, relids))
                {
                        /* Make a copy if we are changing the set. */
                        if (!result)
                                result = bms_copy(relids);

                        result = bms_del_member(result, appinfo->parent_relid);
                        result = bms_add_member(result, appinfo->child_relid);
                }
        }

        /* If we made any changes, return the modified copy. */
        if (result)
                return result;

        /* Otherwise, return the original set without modification. */
        return relids;
}

/*
 * Replace any relid present in top_parent_relids with its child in
 * child_relids. Members of child_relids can be multiple levels below top
 * parent in the partition hierarchy.
 */
Relids
adjust_child_relids_multilevel(PlannerInfo *root, Relids relids,
							   Relids child_relids, Relids top_parent_relids)
{
	AppendRelInfo **appinfos;
	int			nappinfos;
	Relids		parent_relids = NULL;
	Relids		result;
	Relids		tmp_result = NULL;
	int			cnt;

	/*
	 * If the given relids set doesn't contain any of the top parent relids,
	 * it will remain unchanged.
	 */
	if (!bms_overlap(relids, top_parent_relids))
		return relids;

	appinfos = find_appinfos_by_relids(root, child_relids, &nappinfos);

	/* Construct relids set for the immediate parent of the given child. */
	for (cnt = 0; cnt < nappinfos; cnt++)
	{
		AppendRelInfo *appinfo = appinfos[cnt];

		parent_relids = bms_add_member(parent_relids, appinfo->parent_relid);
	}

	/* Recurse if immediate parent is not the top parent. */
	if (!bms_equal(parent_relids, top_parent_relids))
	{
		tmp_result = adjust_child_relids_multilevel(root, relids,
													parent_relids,
													top_parent_relids);
		relids = tmp_result;
	}

	result = adjust_child_relids(relids, nappinfos, appinfos);

	/* Free memory consumed by any intermediate result. */
	if (tmp_result)
		bms_free(tmp_result);
	bms_free(parent_relids);
	pfree(appinfos);

	return result;
}

/*
 * Adjust the targetlist entries of an inherited UPDATE operation
 *
 * The expressions have already been fixed, but we have to make sure that
 * the target resnos match the child table (they may not, in the case of
 * a column that was added after-the-fact by ALTER TABLE).  In some cases
 * this can force us to re-order the tlist to preserve resno ordering.
 * (We do all this work in special cases so that preptlist.c is fast for
 * the typical case.)
 *
 * The given tlist has already been through expression_tree_mutator;
 * therefore the TargetEntry nodes are fresh copies that it's okay to
 * scribble on.
 *
 * Note that this is not needed for INSERT because INSERT isn't inheritable.
 */
static List *
adjust_inherited_tlist(List *tlist, AppendRelInfo *context)
{// #lizard forgives
    bool        changed_it = false;
    ListCell   *tl;
    List       *new_tlist;
    bool        more;
    int            attrno;

    /* This should only happen for an inheritance case, not UNION ALL */
    Assert(OidIsValid(context->parent_reloid));

    /* Scan tlist and update resnos to match attnums of child rel */
    foreach(tl, tlist)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(tl);
        Var           *childvar;

        if (tle->resjunk)
            continue;            /* ignore junk items */

        /* Look up the translation of this column: it must be a Var */
        if (tle->resno <= 0 ||
            tle->resno > list_length(context->translated_vars))
            elog(ERROR, "attribute %d of relation \"%s\" does not exist",
                 tle->resno, get_rel_name(context->parent_reloid));
        childvar = (Var *) list_nth(context->translated_vars, tle->resno - 1);
        if (childvar == NULL || !IsA(childvar, Var))
            elog(ERROR, "attribute %d of relation \"%s\" does not exist",
                 tle->resno, get_rel_name(context->parent_reloid));

        if (tle->resno != childvar->varattno)
        {
            tle->resno = childvar->varattno;
            changed_it = true;
        }
    }

    /*
     * If we changed anything, re-sort the tlist by resno, and make sure
     * resjunk entries have resnos above the last real resno.  The sort
     * algorithm is a bit stupid, but for such a seldom-taken path, small is
     * probably better than fast.
     */
    if (!changed_it)
        return tlist;

    new_tlist = NIL;
    more = true;
    for (attrno = 1; more; attrno++)
    {
        more = false;
        foreach(tl, tlist)
        {
            TargetEntry *tle = (TargetEntry *) lfirst(tl);

            if (tle->resjunk)
                continue;        /* ignore junk items */

            if (tle->resno == attrno)
                new_tlist = lappend(new_tlist, tle);
            else if (tle->resno > attrno)
                more = true;
        }
    }

    foreach(tl, tlist)
    {
        TargetEntry *tle = (TargetEntry *) lfirst(tl);

        if (!tle->resjunk)
            continue;            /* here, ignore non-junk items */

        tle->resno = attrno;
        new_tlist = lappend(new_tlist, tle);
        attrno++;
    }

    return new_tlist;
}

/*
 * adjust_appendrel_attrs_multilevel
 *      Apply Var translations from a toplevel appendrel parent down to a child.
 *
 * In some cases we need to translate expressions referencing a parent relation
 * to reference an appendrel child that's multiple levels removed from it.
 */
Node *
adjust_appendrel_attrs_multilevel(PlannerInfo *root, Node *node,
								  Relids child_relids,
                                  Relids top_parent_relids)
{
    AppendRelInfo **appinfos;
    Bitmapset  *parent_relids = NULL;
    int         nappinfos;
    int         cnt;

    Assert(bms_num_members(child_relids) == bms_num_members(top_parent_relids));

    appinfos = find_appinfos_by_relids(root, child_relids, &nappinfos);
 
    /* Construct relids set for the immediate parent of given child. */
    for (cnt = 0; cnt < nappinfos; cnt++)
{
        AppendRelInfo *appinfo = appinfos[cnt];

        parent_relids = bms_add_member(parent_relids, appinfo->parent_relid);
    }

    /* Recurse if immediate parent is not the top parent. */
    if (!bms_equal(parent_relids, top_parent_relids))
        node = adjust_appendrel_attrs_multilevel(root, node, parent_relids,
                                                 top_parent_relids);

	/* Now translate for this child */
	node = adjust_appendrel_attrs(root, node, nappinfos, appinfos);

    pfree(appinfos);

    return node;
}

/*
 * find_appinfos_by_relids
 *      Find AppendRelInfo structures for all relations specified by relids.
 *
 * The AppendRelInfos are returned in an array, which can be pfree'd by the
 * caller. *nappinfos is set to the number of entries in the array.
 */
AppendRelInfo **
find_appinfos_by_relids(PlannerInfo *root, Relids relids, int *nappinfos)
{
    ListCell   *lc;
    AppendRelInfo **appinfos;
    int         cnt = 0;

    *nappinfos = bms_num_members(relids);
    appinfos = (AppendRelInfo **) palloc(sizeof(AppendRelInfo *) * *nappinfos);

    foreach(lc, root->append_rel_list)
    {
        AppendRelInfo *appinfo = lfirst(lc);

        if (bms_is_member(appinfo->child_relid, relids))
        {
            appinfos[cnt] = appinfo;
            cnt++;

            /* Stop when we have gathered all the AppendRelInfos. */
            if (cnt == *nappinfos)
                return appinfos;
        }
    }

    /* Should have found the entries ... */
    elog(ERROR, "did not find all requested child rels in append_rel_list");
    return NULL;                /* not reached */
}

/*
 * Construct the SpecialJoinInfo for a child-join by translating
 * SpecialJoinInfo for the join between parents. left_relids and right_relids
 * are the relids of left and right side of the join respectively.
 */
SpecialJoinInfo *
build_child_join_sjinfo(PlannerInfo *root, SpecialJoinInfo *parent_sjinfo,
                       Relids left_relids, Relids right_relids)
{
   SpecialJoinInfo *sjinfo = makeNode(SpecialJoinInfo);
   AppendRelInfo **left_appinfos;
   int         left_nappinfos;
   AppendRelInfo **right_appinfos;
   int         right_nappinfos;

   memcpy(sjinfo, parent_sjinfo, sizeof(SpecialJoinInfo));
   left_appinfos = find_appinfos_by_relids(root, left_relids,
                                           &left_nappinfos);
   right_appinfos = find_appinfos_by_relids(root, right_relids,
                                            &right_nappinfos);

   sjinfo->min_lefthand = adjust_child_relids(sjinfo->min_lefthand,
                                              left_nappinfos, left_appinfos);
   sjinfo->min_righthand = adjust_child_relids(sjinfo->min_righthand,
                                               right_nappinfos,
                                               right_appinfos);
   sjinfo->syn_lefthand = adjust_child_relids(sjinfo->syn_lefthand,
                                              left_nappinfos, left_appinfos);
   sjinfo->syn_righthand = adjust_child_relids(sjinfo->syn_righthand,
                                               right_nappinfos,
                                               right_appinfos);
   sjinfo->semi_rhs_exprs = (List *) adjust_appendrel_attrs(root,
                                                            (Node *) sjinfo->semi_rhs_exprs,
                                                            right_nappinfos,
                                                            right_appinfos);

   pfree(left_appinfos);
   pfree(right_appinfos);

   return sjinfo;
}
