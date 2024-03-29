/*-------------------------------------------------------------------------
 *
 * prepare.c
 *      Prepareable SQL statements via PREPARE, EXECUTE and DEALLOCATE
 *
 * This module also implements storage of prepared statements that are
 * accessed via the extended FE/BE query protocol.
 *
 *
 * Copyright (c) 2002-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *      src/backend/commands/prepare.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/createas.h"
#include "commands/prepare.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#ifdef PGXC
#include "pgxc/pgxc.h"
#include "nodes/nodes.h"
#include "pgxc/nodemgr.h"
#include "pgxc/execRemote.h"
#include "catalog/pgxc_node.h"
#include "utils/resowner_private.h"
#endif
#ifdef __TBASE__
#include "commands/vacuum.h"
#endif

/*
 * The hash table in which prepared queries are stored. This is
 * per-backend: query plans are not shared between backends.
 * The keys for this hash table are the arguments to PREPARE and EXECUTE
 * (statement names); the entries are PreparedStatement structs.
 */
static HTAB *prepared_queries = NULL;
#ifdef PGXC
/*
 * The hash table where Datanode prepared statements are stored.
 * The keys are statement names referenced from cached RemoteQuery nodes; the
 * entries are DatanodeStatement structs
 */
static HTAB *datanode_queries = NULL;
#endif

static void InitQueryHashTable(void);
static ParamListInfo EvaluateParams(PreparedStatement *pstmt, List *params,
               const char *queryString, EState *estate);
static Datum build_regtype_array(Oid *param_types, int num_params);

/*
 * Implements the 'PREPARE' utility statement.
 */
void
PrepareQuery(PrepareStmt *stmt, const char *queryString,
             int stmt_location, int stmt_len)
{// #lizard forgives
    RawStmt    *rawstmt;
    CachedPlanSource *plansource;
    Oid           *argtypes = NULL;
    int            nargs;
    Query       *query;
    List       *query_list;
    int            i;

    /*
     * Disallow empty-string statement name (conflicts with protocol-level
     * unnamed statement).
     */
    if (!stmt->name || stmt->name[0] == '\0')
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
                 errmsg("invalid statement name: must not be empty")));

    /*
     * Need to wrap the contained statement in a RawStmt node to pass it to
     * parse analysis.
     *
     * Because parse analysis scribbles on the raw querytree, we must make a
     * copy to ensure we don't modify the passed-in tree.  FIXME someday.
     */
    rawstmt = makeNode(RawStmt);
    rawstmt->stmt = (Node *) copyObject(stmt->query);
    rawstmt->stmt_location = stmt_location;
    rawstmt->stmt_len = stmt_len;

    /*
     * Create the CachedPlanSource before we do parse analysis, since it needs
     * to see the unmodified raw parse tree.
     */
    plansource = CreateCachedPlan(rawstmt, queryString,
#ifdef PGXC
                                  stmt->name,
#endif
                                  CreateCommandTag(stmt->query));

    /* Transform list of TypeNames to array of type OIDs */
    nargs = list_length(stmt->argtypes);

    if (nargs)
    {
        ParseState *pstate;
        ListCell   *l;

        /*
         * typenameTypeId wants a ParseState to carry the source query string.
         * Is it worth refactoring its API to avoid this?
         */
        pstate = make_parsestate(NULL);
        pstate->p_sourcetext = queryString;

        argtypes = (Oid *) palloc(nargs * sizeof(Oid));
        i = 0;

        foreach(l, stmt->argtypes)
        {
            TypeName   *tn = lfirst(l);
            Oid            toid = typenameTypeId(pstate, tn);

            argtypes[i++] = toid;
        }
    }

    /*
     * Analyze the statement using these parameter types (any parameters
     * passed in from above us will not be visible to it), allowing
     * information about unknown parameters to be deduced from context.
     */
    query = parse_analyze_varparams(rawstmt, queryString,
                                    &argtypes, &nargs);

    /*
     * Check that all parameter types were determined.
     */
    for (i = 0; i < nargs; i++)
    {
        Oid            argtype = argtypes[i];

        if (argtype == InvalidOid || argtype == UNKNOWNOID)
            ereport(ERROR,
                    (errcode(ERRCODE_INDETERMINATE_DATATYPE),
                     errmsg("could not determine data type of parameter $%d",
                            i + 1)));
    }

    /*
     * grammar only allows OptimizableStmt, so this check should be redundant
     */
    switch (query->commandType)
    {
        case CMD_SELECT:
        case CMD_INSERT:
        case CMD_UPDATE:
        case CMD_DELETE:
            /* OK */
            break;
        default:
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
                     errmsg("utility statements cannot be prepared")));
            break;
    }

    /* Rewrite the query. The result could be 0, 1, or many queries. */
    query_list = QueryRewrite(query);

    /* Finish filling in the CachedPlanSource */
    CompleteCachedPlan(plansource,
                       query_list,
                       NULL,
                       argtypes,
                       nargs,
                       NULL,
                       NULL,
                       CURSOR_OPT_PARALLEL_OK,    /* allow parallel mode */
                       true);    /* fixed result */

    /*
     * Save the results.
     */
    StorePreparedStatement(stmt->name,
                           plansource,
                           true,
						   false,
						   'N');
}

/*
 * ExecuteQuery --- implement the 'EXECUTE' utility statement.
 *
 * This code also supports CREATE TABLE ... AS EXECUTE.  That case is
 * indicated by passing a non-null intoClause.  The DestReceiver is already
 * set up correctly for CREATE TABLE AS, but we still have to make a few
 * other adjustments here.
 *
 * Note: this is one of very few places in the code that needs to deal with
 * two query strings at once.  The passed-in queryString is that of the
 * EXECUTE, which we might need for error reporting while processing the
 * parameter expressions.  The query_string that we copy from the plan
 * source is that of the original PREPARE.
 */
void
ExecuteQuery(ExecuteStmt *stmt, IntoClause *intoClause,
             const char *queryString, ParamListInfo params,
             DestReceiver *dest, char *completionTag)
{// #lizard forgives
    PreparedStatement *entry;
    CachedPlan *cplan;
    List       *plan_list;
    ParamListInfo paramLI = NULL;
    EState       *estate = NULL;
    Portal        portal;
    char       *query_string;
    int            eflags;
    long        count;

    /* Look it up in the hash table */
    entry = FetchPreparedStatement(stmt->name, true);

    /* Shouldn't find a non-fixed-result cached plan */
    if (!entry->plansource->fixed_result)
        elog(ERROR, "EXECUTE does not support variable-result cached plans");

    /* Evaluate parameters, if any */
    if (entry->plansource->num_params > 0)
    {
        /*
         * Need an EState to evaluate parameters; must not delete it till end
         * of query, in case parameters are pass-by-reference.  Note that the
         * passed-in "params" could possibly be referenced in the parameter
         * expressions.
         */
        estate = CreateExecutorState();
        estate->es_param_list_info = params;
        paramLI = EvaluateParams(entry, stmt->params,
                                 queryString, estate);
    }

    /* Create a new portal to run the query in */
    portal = CreateNewPortal();
    /* Don't display the portal in pg_cursors, it is for internal use only */
    portal->visible = false;

    /* Copy the plan's saved query string into the portal's memory */
    query_string = MemoryContextStrdup(PortalGetHeapMemory(portal),
                                       entry->plansource->query_string);

    /* Replan if needed, and increment plan refcount for portal */
    cplan = GetCachedPlan(entry->plansource, paramLI, false, NULL);
    plan_list = cplan->stmt_list;

    /*
     * For CREATE TABLE ... AS EXECUTE, we must verify that the prepared
     * statement is one that produces tuples.  Currently we insist that it be
     * a plain old SELECT.  In future we might consider supporting other
     * things such as INSERT ... RETURNING, but there are a couple of issues
     * to be settled first, notably how WITH NO DATA should be handled in such
     * a case (do we really want to suppress execution?) and how to pass down
     * the OID-determining eflags (PortalStart won't handle them in such a
     * case, and for that matter it's not clear the executor will either).
     *
     * For CREATE TABLE ... AS EXECUTE, we also have to ensure that the proper
     * eflags and fetch count are passed to PortalStart/PortalRun.
     */
    if (intoClause)
    {
        PlannedStmt *pstmt;

        if (list_length(plan_list) != 1)
            ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                     errmsg("prepared statement is not a SELECT")));
        pstmt = linitial_node(PlannedStmt, plan_list);
        if (pstmt->commandType != CMD_SELECT)
            ereport(ERROR,
                    (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                     errmsg("prepared statement is not a SELECT")));

        /* Set appropriate eflags */
        eflags = GetIntoRelEFlags(intoClause);

        /* And tell PortalRun whether to run to completion or not */
        if (intoClause->skipData)
            count = 0;
        else
            count = FETCH_ALL;
    }
    else
    {
        /* Plain old EXECUTE */
        eflags = 0;
        count = FETCH_ALL;
    }

    PortalDefineQuery(portal,
                      NULL,
                      query_string,
                      entry->plansource->commandTag,
                      plan_list,
                      cplan);

    /*
     * Run the portal as appropriate.
     */
    PortalStart(portal, paramLI, eflags, GetActiveSnapshot());

    (void) PortalRun(portal, count, false, true, dest, dest, completionTag);

    PortalDrop(portal, false);

    if (estate)
        FreeExecutorState(estate);

    /* No need to pfree other memory, MemoryContext will be reset */
}

/*
 * EvaluateParams: evaluate a list of parameters.
 *
 * pstmt: statement we are getting parameters for.
 * params: list of given parameter expressions (raw parser output!)
 * queryString: source text for error messages.
 * estate: executor state to use.
 *
 * Returns a filled-in ParamListInfo -- this can later be passed to
 * CreateQueryDesc(), which allows the executor to make use of the parameters
 * during query execution.
 */
static ParamListInfo
EvaluateParams(PreparedStatement *pstmt, List *params,
               const char *queryString, EState *estate)
{
    Oid           *param_types = pstmt->plansource->param_types;
    int            num_params = pstmt->plansource->num_params;
    int            nparams = list_length(params);
    ParseState *pstate;
    ParamListInfo paramLI;
    List       *exprstates;
    ListCell   *l;
    int            i;

    if (nparams != num_params)
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("wrong number of parameters for prepared statement \"%s\"",
                        pstmt->stmt_name),
                 errdetail("Expected %d parameters but got %d.",
                           num_params, nparams)));

    /* Quick exit if no parameters */
    if (num_params == 0)
        return NULL;

    /*
     * We have to run parse analysis for the expressions.  Since the parser is
     * not cool about scribbling on its input, copy first.
     */
    params = copyObject(params);

    pstate = make_parsestate(NULL);
    pstate->p_sourcetext = queryString;

    i = 0;
    foreach(l, params)
    {
        Node       *expr = lfirst(l);
        Oid            expected_type_id = param_types[i];
        Oid            given_type_id;

        expr = transformExpr(pstate, expr, EXPR_KIND_EXECUTE_PARAMETER);

        given_type_id = exprType(expr);

        expr = coerce_to_target_type(pstate, expr, given_type_id,
                                     expected_type_id, -1,
                                     COERCION_ASSIGNMENT,
                                     COERCE_IMPLICIT_CAST,
                                     -1);

        if (expr == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("parameter $%d of type %s cannot be coerced to the expected type %s",
                            i + 1,
                            format_type_be(given_type_id),
                            format_type_be(expected_type_id)),
                     errhint("You will need to rewrite or cast the expression.")));

        /* Take care of collations in the finished expression. */
        assign_expr_collations(pstate, expr);

        lfirst(l) = expr;
        i++;
    }

    /* Prepare the expressions for execution */
    exprstates = ExecPrepareExprList(params, estate);

    paramLI = (ParamListInfo)
        palloc(offsetof(ParamListInfoData, params) +
               num_params * sizeof(ParamExternData));
    /* we have static list of params, so no hooks needed */
    paramLI->paramFetch = NULL;
    paramLI->paramFetchArg = NULL;
    paramLI->parserSetup = NULL;
    paramLI->parserSetupArg = NULL;
    paramLI->numParams = num_params;
    paramLI->paramMask = NULL;

    i = 0;
    foreach(l, exprstates)
    {
        ExprState  *n = (ExprState *) lfirst(l);
        ParamExternData *prm = &paramLI->params[i];

        prm->ptype = param_types[i];
        prm->pflags = PARAM_FLAG_CONST;
        prm->value = ExecEvalExprSwitchContext(n,
                                               GetPerTupleExprContext(estate),
                                               &prm->isnull);

        i++;
    }

    return paramLI;
}


/*
 * Initialize query hash table upon first use.
 */
static void
InitQueryHashTable(void)
{
    HASHCTL        hash_ctl;

    MemSet(&hash_ctl, 0, sizeof(hash_ctl));

    hash_ctl.keysize = NAMEDATALEN;
    hash_ctl.entrysize = sizeof(PreparedStatement);

    prepared_queries = hash_create("Prepared Queries",
                                   32,
                                   &hash_ctl,
                                   HASH_ELEM);
#ifdef PGXC
    if (IS_PGXC_COORDINATOR)
    {
        MemSet(&hash_ctl, 0, sizeof(hash_ctl));

        hash_ctl.keysize = NAMEDATALEN;
        hash_ctl.entrysize = sizeof(DatanodeStatement) + NumDataNodes * sizeof(int);

        datanode_queries = hash_create("Datanode Queries",
                                       64,
                                       &hash_ctl,
                                       HASH_ELEM);
    }
#endif
}

/*
 * Rebuild query hash table.
 */
void
RebuildDatanodeQueryHashTable(void)
{
	HASHCTL		hash_ctl;
	HASH_SEQ_STATUS seq;
	DatanodeStatement *entry;
	DatanodeStatement *entry_tmp;
	Size               original_entry_size;
	HTAB        *datanode_queries_tmp = NULL;

	if (!IS_PGXC_COORDINATOR || !datanode_queries)
	{
		return;
	}

	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(DatanodeStatement) + NumDataNodes * sizeof(int);

	original_entry_size = hash_get_entry_size(datanode_queries);

	/* node number not changed, no need to rebuild */
	if (original_entry_size == hash_ctl.entrysize)
	{
		return ;
	}

	datanode_queries_tmp = hash_create("Datanode Queries",
	                               64,
	                               &hash_ctl,
	                               HASH_ELEM);
	/* walk over cache */
	hash_seq_init(&seq, datanode_queries);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		/* Now we can copy the hash table entry */
		entry_tmp = (DatanodeStatement  *) hash_search(datanode_queries_tmp, entry->stmt_name,
		                                               HASH_ENTER, NULL);
		memcpy(entry_tmp, entry, original_entry_size);
	}

	hash_destroy(datanode_queries);
	datanode_queries = datanode_queries_tmp;
}

#ifdef PGXC
/*
 * Assign the statement name for all the RemoteQueries in the plan tree, so
 * they use Datanode statements
 */
int
SetRemoteStatementName(Plan *plan, const char *stmt_name, int num_params,
                        Oid *param_types, int n)
{// #lizard forgives
    /* If no plan simply return */
    if (!plan)
        return 0;

    /* Leave if no parameters */
    if (num_params == 0 || !param_types)
        return 0;

    if (IsA(plan, RemoteQuery))
    {
        RemoteQuery *remotequery = (RemoteQuery *) plan;
        DatanodeStatement *entry;
        bool exists;
        char name[NAMEDATALEN];

        /* Nothing to do if parameters are already set for this query */
        if (remotequery->rq_num_params != 0)
            return 0;

        if (stmt_name)
        {
                strcpy(name, stmt_name);
                /*
                 * Append modifier. If resulting string is going to be truncated,
                 * truncate better the base string, otherwise we may enter endless
                 * loop
                 */
                if (n)
                {
                    char modifier[NAMEDATALEN];
                    sprintf(modifier, "__%d", n);
                    /*
                     * if position NAMEDATALEN - strlen(modifier) - 1 is beyond the
                     * base string this is effectively noop, otherwise it truncates
                     * the base string
                     */
                    name[NAMEDATALEN - strlen(modifier) - 1] = '\0';
                    strcat(name, modifier);
                }
                n++;
                hash_search(datanode_queries, name, HASH_FIND, &exists);

            /* If it already exists, that means this plan has just been revalidated. */
            if (!exists)
            {
                entry = (DatanodeStatement *) hash_search(datanode_queries,
                                                  name,
                                                  HASH_ENTER,
                                                  NULL);
                entry->number_of_nodes = 0;
            }

            remotequery->statement = pstrdup(name);
        }
        else if (remotequery->statement)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("Passing parameters in PREPARE statement is not supported")));

        remotequery->rq_num_params = num_params;
        remotequery->rq_param_types = param_types;
    }
    else if (IsA(plan, ModifyTable))
    {
        ModifyTable    *mt_plan = (ModifyTable *)plan;
        /* For ModifyTable plan recurse into each of the plans underneath */
        ListCell    *l;
        foreach(l, mt_plan->plans)
        {
            Plan *plan = lfirst(l);
            n = SetRemoteStatementName(plan, stmt_name, num_params,
                                        param_types, n);
        }
    }

    if (innerPlan(plan))
        n = SetRemoteStatementName(innerPlan(plan), stmt_name, num_params,
                                    param_types, n);

    if (outerPlan(plan))
        n = SetRemoteStatementName(outerPlan(plan), stmt_name, num_params,
                                    param_types, n);

    return n;
}
#endif

/*
 * Store all the data pertaining to a query in the hash table using
 * the specified key.  The passed CachedPlanSource should be "unsaved"
 * in case we get an error here; we'll save it once we've created the hash
 * table entry.
 */
void
StorePreparedStatement(const char *stmt_name,
                       CachedPlanSource *plansource,
                       bool from_sql,
					   bool use_resowner,
					   const char need_rewrite)
{
	PreparedStatement *entry;
	TimestampTz cur_ts = GetCurrentStatementStartTimestamp();
	bool		found;

	/* Initialize the hash table, if necessary */
	if (!prepared_queries)
		InitQueryHashTable();

	/* Add entry to hash table */
	entry = (PreparedStatement *) hash_search(prepared_queries,
											  stmt_name,
											  HASH_ENTER,
											  &found);

	/* Shouldn't get a duplicate entry */
	if (found)
	{
		if (need_rewrite == 'Y' &&
			plansource->commandTag == entry->plansource->commandTag &&
			strcmp(plansource->query_string, entry->plansource->query_string) != 0)
		{
			entry->plansource->query_string = plansource->query_string;
		}
		else if (!(plansource->commandTag == entry->plansource->commandTag &&
				strcmp(plansource->query_string, entry->plansource->query_string) == 0))
		{
			ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_PSTATEMENT),
					errmsg("prepared statement \"%s\" already exists, and plansource is not the same.",
					stmt_name)));
		}
		else
		{
			elog(LOG, " \"%s\" already exists in prepared_queries, skip it.", stmt_name);
			return ;
		}
	}

	/* Fill in the hash table entry */
	entry->plansource = plansource;
	entry->from_sql = from_sql;
	entry->prepare_time = cur_ts;
	entry->use_resowner = use_resowner;

	/* Now it's safe to move the CachedPlanSource to permanent memory */
	SaveCachedPlan(plansource);

#ifdef XCP	
	if (use_resowner)
	{
		ResourceOwnerEnlargePreparedStmts(CurTransactionResourceOwner);
		ResourceOwnerRememberPreparedStmt(CurTransactionResourceOwner,
				entry->stmt_name);
	}
#endif		
}

/*
 * Lookup an existing query in the hash table. If the query does not
 * actually exist, throw ereport(ERROR) or return NULL per second parameter.
 *
 * Note: this does not force the referenced plancache entry to be valid,
 * since not all callers care.
 */
PreparedStatement *
FetchPreparedStatement(const char *stmt_name, bool throwError)
{
    PreparedStatement *entry;

    /*
     * If the hash table hasn't been initialized, it can't be storing
     * anything, therefore it couldn't possibly store our plan.
     */
    if (prepared_queries)
        entry = (PreparedStatement *) hash_search(prepared_queries,
                                                  stmt_name,
                                                  HASH_FIND,
                                                  NULL);
    else
        entry = NULL;

    if (!entry && throwError)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_PSTATEMENT),
                 errmsg("prepared statement \"%s\" does not exist",
                        stmt_name)));

    return entry;
}

/*
 * Given a prepared statement, determine the result tupledesc it will
 * produce.  Returns NULL if the execution will not return tuples.
 *
 * Note: the result is created or copied into current memory context.
 */
TupleDesc
FetchPreparedStatementResultDesc(PreparedStatement *stmt)
{
    /*
     * Since we don't allow prepared statements' result tupdescs to change,
     * there's no need to worry about revalidating the cached plan here.
     */
    Assert(stmt->plansource->fixed_result);
    if (stmt->plansource->resultDesc)
        return CreateTupleDescCopy(stmt->plansource->resultDesc);
    else
        return NULL;
}

/*
 * Given a prepared statement that returns tuples, extract the query
 * targetlist.  Returns NIL if the statement doesn't have a determinable
 * targetlist.
 *
 * Note: this is pretty ugly, but since it's only used in corner cases like
 * Describe Statement on an EXECUTE command, we don't worry too much about
 * efficiency.
 */
List *
FetchPreparedStatementTargetList(PreparedStatement *stmt)
{
    List       *tlist;

    /* Get the plan's primary targetlist */
    tlist = CachedPlanGetTargetList(stmt->plansource, NULL);

    /* Copy into caller's context in case plan gets invalidated */
    return copyObject(tlist);
}

/*
 * Implements the 'DEALLOCATE' utility statement: deletes the
 * specified plan from storage.
 */
void
DeallocateQuery(DeallocateStmt *stmt)
{
    if (stmt->name)
        DropPreparedStatement(stmt->name, true);
    else
        DropAllPreparedStatements();
}

/*
 * Internal version of DEALLOCATE
 *
 * If showError is false, dropping a nonexistent statement is a no-op.
 */
void
DropPreparedStatement(const char *stmt_name, bool showError)
{
    PreparedStatement *entry;

    /* Find the query's hash table entry; raise error if wanted */
    entry = FetchPreparedStatement(stmt_name, showError);

    if (entry)
    {
#ifdef XCP
	    /* if a process SharedQueueRelease in DropCachedPlan, this SharedQueue
	     * Can be created by another process, and SharedQueueDisconnectConsumer
	     * will change the SharedQueue of another process's status,
	     * so let SharedQueueDisconnectConsumer be in front of DropCachedPlan */
        SharedQueueDisconnectConsumer(entry->stmt_name);
#endif
        /* Release the plancache entry */
        DropCachedPlan(entry->plansource);

        /* Now we can remove the hash table entry */
        hash_search(prepared_queries, entry->stmt_name, HASH_REMOVE, NULL);

#ifdef XCP
        DropDatanodeStatement(entry->stmt_name);
        if (entry->use_resowner)
            ResourceOwnerForgetPreparedStmt(CurTransactionResourceOwner,
                    entry->stmt_name);
#endif
#ifdef __TBASE__
        if (distributed_query_analyze)
        {
            if (IS_PGXC_DATANODE)
            {
                DropQueryAnalyzeInfo(stmt_name);
            }
        }
#endif
    }
}

/*
 * Drop all cached statements.
 */
void
DropAllPreparedStatements(void)
{// #lizard forgives
    HASH_SEQ_STATUS seq;
    PreparedStatement *entry;

    /* nothing cached */
    if (!prepared_queries)
        return;

    /* walk over cache */
    hash_seq_init(&seq, prepared_queries);
    while ((entry = hash_seq_search(&seq)) != NULL)
    {
        /* Release the plancache entry */
        DropCachedPlan(entry->plansource);

        /* Now we can remove the hash table entry */
        hash_search(prepared_queries, entry->stmt_name, HASH_REMOVE, NULL);

#ifdef XCP
#ifdef __TBASE__
        if (entry->use_resowner && CurTransactionResourceOwner)
#else
        if (entry->use_resowner)
#endif
            ResourceOwnerForgetPreparedStmt(CurTransactionResourceOwner,
                    entry->stmt_name);
#endif        
    }
}

/*
 * Implements the 'EXPLAIN EXECUTE' utility statement.
 *
 * "into" is NULL unless we are doing EXPLAIN CREATE TABLE AS EXECUTE,
 * in which case executing the query should result in creating that table.
 *
 * Note: the passed-in queryString is that of the EXPLAIN EXECUTE,
 * not the original PREPARE; we get the latter string from the plancache.
 */
void
ExplainExecuteQuery(ExecuteStmt *execstmt, IntoClause *into, ExplainState *es,
                    const char *queryString, ParamListInfo params,
                    QueryEnvironment *queryEnv)
{
    PreparedStatement *entry;
    const char *query_string;
    CachedPlan *cplan;
    List       *plan_list;
    ListCell   *p;
    ParamListInfo paramLI = NULL;
    EState       *estate = NULL;
    instr_time    planstart;
    instr_time    planduration;

    INSTR_TIME_SET_CURRENT(planstart);

    /* Look it up in the hash table */
    entry = FetchPreparedStatement(execstmt->name, true);

    /* Shouldn't find a non-fixed-result cached plan */
    if (!entry->plansource->fixed_result)
        elog(ERROR, "EXPLAIN EXECUTE does not support variable-result cached plans");

    query_string = entry->plansource->query_string;

    /* Evaluate parameters, if any */
    if (entry->plansource->num_params)
    {
        /*
         * Need an EState to evaluate parameters; must not delete it till end
         * of query, in case parameters are pass-by-reference.  Note that the
         * passed-in "params" could possibly be referenced in the parameter
         * expressions.
         */
        estate = CreateExecutorState();
        estate->es_param_list_info = params;
        paramLI = EvaluateParams(entry, execstmt->params,
                                 queryString, estate);
    }

    /* Replan if needed, and acquire a transient refcount */
    cplan = GetCachedPlan(entry->plansource, paramLI, true, queryEnv);

    INSTR_TIME_SET_CURRENT(planduration);
    INSTR_TIME_SUBTRACT(planduration, planstart);

    plan_list = cplan->stmt_list;

    /* Explain each query */
    foreach(p, plan_list)
    {
        PlannedStmt *pstmt = lfirst_node(PlannedStmt, p);

        if (pstmt->commandType != CMD_UTILITY)
            ExplainOnePlan(pstmt, into, es, query_string, paramLI, queryEnv,
                           &planduration);
        else
            ExplainOneUtility(pstmt->utilityStmt, into, es, query_string,
                              paramLI, queryEnv);

        /* No need for CommandCounterIncrement, as ExplainOnePlan did it */

        /* Separate plans with an appropriate separator */
        if (lnext(p) != NULL)
            ExplainSeparatePlans(es);
    }

    if (estate)
        FreeExecutorState(estate);

    ReleaseCachedPlan(cplan, true);
}

/*
 * This set returning function reads all the prepared statements and
 * returns a set of (name, statement, prepare_time, param_types, from_sql).
 */
Datum
pg_prepared_statement(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc    tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext per_query_ctx;
    MemoryContext oldcontext;

    /* check to see if caller supports us returning a tuplestore */
    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required, but it is not " \
                        "allowed in this context")));

    /* need to build tuplestore in query context */
    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);

    /*
     * build tupdesc for result tuples. This must match the definition of the
     * pg_prepared_statements view in system_views.sql
     */
    tupdesc = CreateTemplateTupleDesc(5, false);
    TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
                       TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 2, "statement",
                       TEXTOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 3, "prepare_time",
                       TIMESTAMPTZOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 4, "parameter_types",
                       REGTYPEARRAYOID, -1, 0);
    TupleDescInitEntry(tupdesc, (AttrNumber) 5, "from_sql",
                       BOOLOID, -1, 0);

    /*
     * We put all the tuples into a tuplestore in one scan of the hashtable.
     * This avoids any issue of the hashtable possibly changing between calls.
     */
    tupstore =
        tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random,
                              false, work_mem);

    /* generate junk in short-term context */
    MemoryContextSwitchTo(oldcontext);

    /* hash table might be uninitialized */
    if (prepared_queries)
    {
        HASH_SEQ_STATUS hash_seq;
        PreparedStatement *prep_stmt;

        hash_seq_init(&hash_seq, prepared_queries);
        while ((prep_stmt = hash_seq_search(&hash_seq)) != NULL)
        {
            Datum        values[5];
            bool        nulls[5];

            MemSet(nulls, 0, sizeof(nulls));

            values[0] = CStringGetTextDatum(prep_stmt->stmt_name);
            values[1] = CStringGetTextDatum(prep_stmt->plansource->query_string);
            values[2] = TimestampTzGetDatum(prep_stmt->prepare_time);
            values[3] = build_regtype_array(prep_stmt->plansource->param_types,
                                            prep_stmt->plansource->num_params);
            values[4] = BoolGetDatum(prep_stmt->from_sql);

            tuplestore_putvalues(tupstore, tupdesc, values, nulls);
        }
    }

    /* clean up and return the tuplestore */
    tuplestore_donestoring(tupstore);

    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;

    return (Datum) 0;
}

/*
 * This utility function takes a C array of Oids, and returns a Datum
 * pointing to a one-dimensional Postgres array of regtypes. An empty
 * array is returned as a zero-element array, not NULL.
 */
static Datum
build_regtype_array(Oid *param_types, int num_params)
{
    Datum       *tmp_ary;
    ArrayType  *result;
    int            i;

    tmp_ary = (Datum *) palloc(num_params * sizeof(Datum));

    for (i = 0; i < num_params; i++)
        tmp_ary[i] = ObjectIdGetDatum(param_types[i]);

    /* XXX: this hardcodes assumptions about the regtype type */
    result = construct_array(tmp_ary, num_params, REGTYPEOID, 4, true, 'i');
    return PointerGetDatum(result);
}


#ifdef PGXC
DatanodeStatement *
FetchDatanodeStatement(const char *stmt_name, bool throwError)
{
    DatanodeStatement *entry;

    /*
     * If the hash table hasn't been initialized, it can't be storing
     * anything, therefore it couldn't possibly store our plan.
     */
    if (datanode_queries)
        entry = (DatanodeStatement *) hash_search(datanode_queries, stmt_name, HASH_FIND, NULL);
    else
        entry = NULL;

    /* Report error if entry is not found */
    if (!entry && throwError)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_PSTATEMENT),
                 errmsg("datanode statement \"%s\" does not exist",
                        stmt_name)));

    return entry;
}

/*
 * Drop Datanode statement and close it on nodes if active
 */
void
DropDatanodeStatement(const char *stmt_name)
{
    DatanodeStatement *entry;

    entry = FetchDatanodeStatement(stmt_name, false);
    if (entry)
    {
        int i;
        List *nodelist = NIL;

        /* make a List of integers from node numbers */
        for (i = 0; i < entry->number_of_nodes; i++)
            nodelist = lappend_int(nodelist, entry->dns_node_indices[i]);
        entry->number_of_nodes = 0;

        ExecCloseRemoteStatement(stmt_name, nodelist);

        hash_search(datanode_queries, entry->stmt_name, HASH_REMOVE, NULL);
    }
}


/*
 * Return true if there is at least one active Datanode statement, so acquired
 * Datanode connections should not be released
 */
bool
HaveActiveDatanodeStatements(void)
{
    HASH_SEQ_STATUS seq;
    DatanodeStatement *entry;

    /* nothing cached */
    if (!datanode_queries)
        return false;

    /* walk over cache */
    hash_seq_init(&seq, datanode_queries);
    while ((entry = hash_seq_search(&seq)) != NULL)
    {
        /* Stop walking and return true */
        if (entry->number_of_nodes > 0)
        {
            hash_seq_term(&seq);
            return true;
        }
    }
    /* nothing found */
    return false;
}


/*
 * Mark Datanode statement as active on specified node
 * Return true if statement has already been active on the node and can be used
 * Returns false if statement has not been active on the node and should be
 * prepared on the node
 */
bool
ActivateDatanodeStatementOnNode(const char *stmt_name, int nodeidx)
{
    DatanodeStatement *entry;
    int i;

    /* find the statement in cache */
    entry = FetchDatanodeStatement(stmt_name, true);

    /* see if statement already active on the node */
    for (i = 0; i < entry->number_of_nodes; i++)
		if (entry->dns_node_indices[i] == nodeidx)
            return true;

    /* statement is not active on the specified node append item to the list */
	entry->dns_node_indices[entry->number_of_nodes++] = nodeidx;
    return false;
}


/*
 * Mark datanode statement as inactive on specified node
 */
void
InactivateDatanodeStatementOnNode(int nodeidx)
{
	HASH_SEQ_STATUS seq;
	DatanodeStatement *entry;
	int i;

	/* nothing cached */
	if (!datanode_queries)
		return;

	/* walk over cache */
	hash_seq_init(&seq, datanode_queries);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		/* see if statement already active on the node */
		for (i = 0; i < entry->number_of_nodes; i++)
		{
			if (entry->dns_node_indices[i] == nodeidx)
			{
				elog(DEBUG5, "InactivateDatanodeStatementOnNode: node index %d, "
						"number_of_nodes %d, statement name %s", nodeidx,
						entry->number_of_nodes, entry->stmt_name);

				/* remove nodeidx from list */
				entry->number_of_nodes--;
				if (i < entry->number_of_nodes)
				{
					entry->dns_node_indices[i] =
						entry->dns_node_indices[entry->number_of_nodes];
				}
				break;
			}
		}
	}
}

#endif
#ifdef __TBASE__
/* prepare remoteDML statement on coordinator */
void
PrepareRemoteDMLStatement(bool upsert, char *stmt, 
                                    char *select_stmt, char *update_stmt)
{
    bool exists;
    DatanodeStatement *entry;
    
    /* Initialize the hash table, if necessary */
    if (!prepared_queries)
        InitQueryHashTable();

    hash_search(datanode_queries, stmt, HASH_FIND, &exists);

    if (!exists)
    {
        entry = (DatanodeStatement *) hash_search(datanode_queries,
                                  stmt,
                                  HASH_ENTER,
                                  NULL);
        entry->number_of_nodes = 0;
    }

    if (upsert)
    {
        if (select_stmt)
        {
            hash_search(datanode_queries, select_stmt, HASH_FIND, &exists);

            if (!exists)
            {
                entry = (DatanodeStatement *) hash_search(datanode_queries,
                                          select_stmt,
                                          HASH_ENTER,
                                          NULL);
                entry->number_of_nodes = 0;
            }
        }

        if (update_stmt)
        {
            hash_search(datanode_queries, update_stmt, HASH_FIND, &exists);

            if (!exists)
            {
                entry = (DatanodeStatement *) hash_search(datanode_queries,
                                          update_stmt,
                                          HASH_ENTER,
                                          NULL);
                entry->number_of_nodes = 0;
            }
        }
    }
}

void
DropRemoteDMLStatement(char *stmt, char *update_stmt)
{
    if (!datanode_queries)
        return;

    if (stmt)
    {
        hash_search(datanode_queries, stmt, HASH_REMOVE, NULL);
    }

    if (update_stmt)
    {
        hash_search(datanode_queries, update_stmt, HASH_REMOVE, NULL);
    }
}
#endif
