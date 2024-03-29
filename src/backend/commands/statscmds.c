/*-------------------------------------------------------------------------
 *
 * statscmds.c
 *      Commands for creating and altering extended statistics objects
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *      src/backend/commands/statscmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_statistic_ext.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "statistics/statistics.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* qsort comparator for the attnums in CreateStatistics */
static int
compare_int16(const void *a, const void *b)
{
    int            av = *(const int16 *) a;
    int            bv = *(const int16 *) b;

    /* this can't overflow if int is wider than int16 */
    return (av - bv);
}

/*
 *        CREATE STATISTICS
 */
ObjectAddress
CreateStatistics(CreateStatsStmt *stmt)
{
	int16		attnums[STATS_MAX_DIMENSIONS];
#ifdef __TBASE__
	int16		attnums_ori[STATS_MAX_DIMENSIONS];
#endif
	int			numcols = 0;
	char	   *namestr;
	NameData	stxname;
	Oid			statoid;
	Oid			namespaceId;
	Oid			stxowner = GetUserId();
	HeapTuple	htup;
	Datum		values[Natts_pg_statistic_ext];
	bool		nulls[Natts_pg_statistic_ext];
	int2vector *stxkeys;
	Relation	statrel;
	Relation	rel = NULL;
	Oid			relid;
	ObjectAddress parentobject,
				myself;
#ifdef __TBASE__
    Datum		types[3];		/* one for each possible type of statistic */
#else
	Datum		types[2];		/* one for each possible type of statistic */
#endif

	int			ntypes;
	ArrayType  *stxkind;
	bool		build_ndistinct;
	bool		build_dependencies;
#ifdef __TBASE__
	bool		build_subset;
#endif
	bool		requested_type = false;
	int			i;
	ListCell   *cell;

	Assert(IsA(stmt, CreateStatsStmt));

	/* resolve the pieces of the name (namespace etc.) */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->defnames, &namestr);
	namestrcpy(&stxname, namestr);

	/*
	 * Deal with the possibility that the statistics object already exists.
	 */
	if (SearchSysCacheExists2(STATEXTNAMENSP,
							  NameGetDatum(&stxname),
							  ObjectIdGetDatum(namespaceId)))
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("statistics object \"%s\" already exists, skipping",
							namestr)));
			return InvalidObjectAddress;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("statistics object \"%s\" already exists", namestr)));
	}

	/*
	 * Examine the FROM clause.  Currently, we only allow it to be a single
	 * simple table, but later we'll probably allow multiple tables and JOIN
	 * syntax.  The grammar is already prepared for that, so we have to check
	 * here that what we got is what we can support.
	 */
	if (list_length(stmt->relations) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only a single relation is allowed in CREATE STATISTICS")));

	foreach(cell, stmt->relations)
	{
		Node	   *rln = (Node *) lfirst(cell);

		if (!IsA(rln, RangeVar))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("only a single relation is allowed in CREATE STATISTICS")));

		/*
		 * CREATE STATISTICS will influence future execution plans but does
		 * not interfere with currently executing plans.  So it should be
		 * enough to take only ShareUpdateExclusiveLock on relation,
		 * conflicting with ANALYZE and other DDL that sets statistical
		 * information, but not with normal queries.
		 */
		rel = relation_openrv((RangeVar *) rln, ShareUpdateExclusiveLock);

		/* Restrict to allowed relation types */
		if (rel->rd_rel->relkind != RELKIND_RELATION &&
			rel->rd_rel->relkind != RELKIND_MATVIEW &&
			rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
			rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("relation \"%s\" is not a table, foreign table, or materialized view",
							RelationGetRelationName(rel))));

		/* You must own the relation to create stats on it */
		if (!pg_class_ownercheck(RelationGetRelid(rel), stxowner))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
						   RelationGetRelationName(rel));
	}

	Assert(rel);
	relid = RelationGetRelid(rel);

	/*
	 * Currently, we only allow simple column references in the expression
	 * list.  That will change someday, and again the grammar already supports
	 * it so we have to enforce restrictions here.  For now, we can convert
	 * the expression list to a simple array of attnums.  While at it, enforce
	 * some constraints.
	 */
	foreach(cell, stmt->exprs)
	{
		Node	   *expr = (Node *) lfirst(cell);
		ColumnRef  *cref;
		char	   *attname;
		HeapTuple	atttuple;
		Form_pg_attribute attForm;
		TypeCacheEntry *type;

		if (!IsA(expr, ColumnRef))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("only simple column references are allowed in CREATE STATISTICS")));
		cref = (ColumnRef *) expr;

		if (list_length(cref->fields) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("only simple column references are allowed in CREATE STATISTICS")));
		attname = strVal((Value *) linitial(cref->fields));

		atttuple = SearchSysCacheAttName(relid, attname);
		if (!HeapTupleIsValid(atttuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" referenced in statistics does not exist",
							attname)));
		attForm = (Form_pg_attribute) GETSTRUCT(atttuple);

		/* Disallow use of system attributes in extended stats */
		if (attForm->attnum <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("statistics creation on system columns is not supported")));

		/* Disallow data types without a less-than operator */
		type = lookup_type_cache(attForm->atttypid, TYPECACHE_LT_OPR);
		if (type->lt_opr == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" cannot be used in statistics because its type has no default btree operator class",
							attname)));

		/* Make sure no more than STATS_MAX_DIMENSIONS columns are used */
		if (numcols >= STATS_MAX_DIMENSIONS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot have more than %d columns in statistics",
							STATS_MAX_DIMENSIONS)));

		attnums[numcols] = attForm->attnum;
#ifdef __TBASE__
		attnums_ori[numcols] = attForm->attnum;
#endif
		numcols++;
		ReleaseSysCache(atttuple);
	}

	/*
	 * Check that at least two columns were specified in the statement. The
	 * upper bound was already checked in the loop above.
	 */
	if (numcols < 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("extended statistics require at least 2 columns")));

	/*
	 * Sort the attnums, which makes detecting duplicates somewhat easier, and
	 * it does not hurt (it does not affect the efficiency, unlike for
	 * indexes, for example).
	 */
	qsort(attnums, numcols, sizeof(int16), compare_int16);

	/*
	 * Check for duplicates in the list of columns. The attnums are sorted so
	 * just check consecutive elements.
	 */
	for (i = 1; i < numcols; i++)
	{
		if (attnums[i] == attnums[i - 1])
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_COLUMN),
					 errmsg("duplicate column name in statistics definition")));
	}

	/* Form an int2vector representation of the sorted column list */
	stxkeys = buildint2vector(attnums, numcols);

	/*
	 * Parse the statistics types.
	 */
	build_ndistinct = false;
	build_dependencies = false;
#ifdef __TBASE__
	build_subset = false;
#endif
	foreach(cell, stmt->stat_types)
	{
		char	   *type = strVal((Value *) lfirst(cell));

		if (strcmp(type, "ndistinct") == 0)
		{
			build_ndistinct = true;
			requested_type = true;
		}
		else if (strcmp(type, "dependencies") == 0)
		{
			build_dependencies = true;
			requested_type = true;
		}
#ifdef __TBASE__
		else if (strcmp(type, "subset") == 0)
		{
			if (list_length(stmt->exprs) != 2)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("subset statistics require exactly 2 columns")));
			}

			build_subset = true;
			requested_type = true;

			/*
			 * The original stmt expr order implies the relation between them,
			 * thus we need to keep the original order stored.
			 */
			stxkeys = buildint2vector(attnums_ori, numcols);
		}
#endif
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized statistic type \"%s\"",
							type)));
	}
	/* If no statistic type was specified, build them all. */
	if (!requested_type)
	{
		build_ndistinct = true;
		build_dependencies = true;
#ifdef __TBASE__
		/* No need to build user defined knowledge */
		build_subset = false;
#endif
	}

	/* construct the char array of enabled statistic types */
	ntypes = 0;
	if (build_ndistinct)
		types[ntypes++] = CharGetDatum(STATS_EXT_NDISTINCT);
	if (build_dependencies)
		types[ntypes++] = CharGetDatum(STATS_EXT_DEPENDENCIES);
#ifdef __TBASE__
	/*
	 * User defined subset hint should not coexists with other
	 * types. Thus we don't need to extend the size of 'types'
	 * array.
	 */
	if (build_subset)
		types[ntypes++] = CharGetDatum(STATS_EXT_SUBSET);
#endif
	Assert(ntypes > 0 && ntypes <= lengthof(types));
	stxkind = construct_array(types, ntypes, CHAROID, 1, true, 'c');

	/*
	 * Everything seems fine, so let's build the pg_statistic_ext tuple.
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	values[Anum_pg_statistic_ext_stxrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_statistic_ext_stxname - 1] = NameGetDatum(&stxname);
	values[Anum_pg_statistic_ext_stxnamespace - 1] = ObjectIdGetDatum(namespaceId);
	values[Anum_pg_statistic_ext_stxowner - 1] = ObjectIdGetDatum(stxowner);
	values[Anum_pg_statistic_ext_stxkeys - 1] = PointerGetDatum(stxkeys);
	values[Anum_pg_statistic_ext_stxkind - 1] = PointerGetDatum(stxkind);

	/* no statistics built yet */
	nulls[Anum_pg_statistic_ext_stxndistinct - 1] = true;
	nulls[Anum_pg_statistic_ext_stxdependencies - 1] = true;
#ifdef __TBASE__
	nulls[Anum_pg_statistic_ext_stxsubset - 1] = true;
#endif

	/* insert it into pg_statistic_ext */
	statrel = heap_open(StatisticExtRelationId, RowExclusiveLock);
	htup = heap_form_tuple(statrel->rd_att, values, nulls);
	statoid = CatalogTupleInsert(statrel, htup);
	heap_freetuple(htup);
	relation_close(statrel, RowExclusiveLock);

	/*
	 * Invalidate relcache so that others see the new statistics object.
	 */
	CacheInvalidateRelcache(rel);

	relation_close(rel, NoLock);

	/*
	 * Add an AUTO dependency on each column used in the stats, so that the
	 * stats object goes away if any or all of them get dropped.
	 */
	ObjectAddressSet(myself, StatisticExtRelationId, statoid);

	for (i = 0; i < numcols; i++)
	{
		ObjectAddressSubSet(parentobject, RelationRelationId, relid, attnums[i]);
		recordDependencyOn(&myself, &parentobject, DEPENDENCY_AUTO);
	}

	/*
	 * Also add dependencies on namespace and owner.  These are required
	 * because the stats object might have a different namespace and/or owner
	 * than the underlying table(s).
	 */
	ObjectAddressSet(parentobject, NamespaceRelationId, namespaceId);
	recordDependencyOn(&myself, &parentobject, DEPENDENCY_NORMAL);

	recordDependencyOnOwner(StatisticExtRelationId, statoid, stxowner);

	/*
	 * XXX probably there should be a recordDependencyOnCurrentExtension call
	 * here too, but we'd have to add support for ALTER EXTENSION ADD/DROP
	 * STATISTICS, which is more work than it seems worth.
	 */

	/* Return stats object's address */
	return myself;
}

/*
 * Guts of statistics object deletion.
 */
void
RemoveStatisticsById(Oid statsOid)
{
    Relation    relation;
    HeapTuple    tup;
    Form_pg_statistic_ext statext;
    Oid            relid;

    /*
     * Delete the pg_statistic_ext tuple.  Also send out a cache inval on the
     * associated table, so that dependent plans will be rebuilt.
     */
    relation = heap_open(StatisticExtRelationId, RowExclusiveLock);

    tup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statsOid));

    if (!HeapTupleIsValid(tup)) /* should not happen */
        elog(ERROR, "cache lookup failed for statistics object %u", statsOid);

    statext = (Form_pg_statistic_ext) GETSTRUCT(tup);
    relid = statext->stxrelid;

    CacheInvalidateRelcacheByRelid(relid);

    CatalogTupleDelete(relation, &tup->t_self);

    ReleaseSysCache(tup);

    heap_close(relation, RowExclusiveLock);
}

/*
 * Update a statistics object for ALTER COLUMN TYPE on a source column.
 *
 * This could throw an error if the type change can't be supported.
 * If it can be supported, but the stats must be recomputed, a likely choice
 * would be to set the relevant column(s) of the pg_statistic_ext tuple to
 * null until the next ANALYZE.  (Note that the type change hasn't actually
 * happened yet, so one option that's *not* on the table is to recompute
 * immediately.)
 */
void
UpdateStatisticsForTypeChange(Oid statsOid, Oid relationOid, int attnum,
                              Oid oldColumnType, Oid newColumnType)
{
    /*
     * Currently, we don't actually need to do anything here.  For both
     * ndistinct and functional-dependencies stats, the on-disk representation
     * is independent of the source column data types, and it is plausible to
     * assume that the old statistic values will still be good for the new
     * column contents.  (Obviously, if the ALTER COLUMN TYPE has a USING
     * expression that substantially alters the semantic meaning of the column
     * values, this assumption could fail.  But that seems like a corner case
     * that doesn't justify zapping the stats in common cases.)
     *
     * Future types of extended stats will likely require us to work harder.
     */
}
