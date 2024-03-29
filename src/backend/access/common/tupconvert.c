/*-------------------------------------------------------------------------
 *
 * tupconvert.c
 *      Tuple conversion support.
 *
 * These functions provide conversion between rowtypes that are logically
 * equivalent but might have columns in a different order or different sets
 * of dropped columns.  There is some overlap of functionality with the
 * executor's "junkfilter" routines, but these functions work on bare
 * HeapTuples rather than TupleTableSlots.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *      src/backend/access/common/tupconvert.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupconvert.h"
#include "utils/builtins.h"


/*
 * The conversion setup routines have the following common API:
 *
 * The setup routine checks whether the given source and destination tuple
 * descriptors are logically compatible.  If not, it throws an error.
 * If so, it returns NULL if they are physically compatible (ie, no conversion
 * is needed), else a TupleConversionMap that can be used by do_convert_tuple
 * to perform the conversion.
 *
 * The TupleConversionMap, if needed, is palloc'd in the caller's memory
 * context.  Also, the given tuple descriptors are referenced by the map,
 * so they must survive as long as the map is needed.
 *
 * The caller must supply a suitable primary error message to be used if
 * a compatibility error is thrown.  Recommended coding practice is to use
 * gettext_noop() on this string, so that it is translatable but won't
 * actually be translated unless the error gets thrown.
 *
 *
 * Implementation notes:
 *
 * The key component of a TupleConversionMap is an attrMap[] array with
 * one entry per output column.  This entry contains the 1-based index of
 * the corresponding input column, or zero to force a NULL value (for
 * a dropped output column).  The TupleConversionMap also contains workspace
 * arrays.
 */


/*
 * Set up for tuple conversion, matching input and output columns by
 * position.  (Dropped columns are ignored in both input and output.)
 *
 * Note: the errdetail messages speak of indesc as the "returned" rowtype,
 * outdesc as the "expected" rowtype.  This is okay for current uses but
 * might need generalization in future.
 */
TupleConversionMap *
convert_tuples_by_position(TupleDesc indesc,
                           TupleDesc outdesc,
                           const char *msg)
{// #lizard forgives
    TupleConversionMap *map;
    AttrNumber *attrMap;
    int            nincols;
    int            noutcols;
    int            n;
    int            i;
    int            j;
    bool        same;

    /* Verify compatibility and prepare attribute-number map */
    n = outdesc->natts;
    attrMap = (AttrNumber *) palloc0(n * sizeof(AttrNumber));
    j = 0;                        /* j is next physical input attribute */
    nincols = noutcols = 0;        /* these count non-dropped attributes */
    same = true;
    for (i = 0; i < n; i++)
    {
        Form_pg_attribute att = outdesc->attrs[i];
        Oid            atttypid;
        int32        atttypmod;

        if (att->attisdropped)
            continue;            /* attrMap[i] is already 0 */
        noutcols++;
        atttypid = att->atttypid;
        atttypmod = att->atttypmod;
        for (; j < indesc->natts; j++)
        {
            att = indesc->attrs[j];
            if (att->attisdropped)
                continue;
            nincols++;
            /* Found matching column, check type */
            if (atttypid != att->atttypid ||
                (atttypmod != att->atttypmod && atttypmod >= 0))
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg_internal("%s", _(msg)),
                         errdetail("Returned type %s does not match expected type %s in column %d.",
                                   format_type_with_typemod(att->atttypid,
                                                            att->atttypmod),
                                   format_type_with_typemod(atttypid,
                                                            atttypmod),
                                   noutcols)));
            attrMap[i] = (AttrNumber) (j + 1);
            j++;
            break;
        }
        if (attrMap[i] == 0)
            same = false;        /* we'll complain below */
    }

    /* Check for unused input columns */
    for (; j < indesc->natts; j++)
    {
        if (indesc->attrs[j]->attisdropped)
            continue;
        nincols++;
        same = false;            /* we'll complain below */
    }

    /* Report column count mismatch using the non-dropped-column counts */
    if (!same)
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg_internal("%s", _(msg)),
                 errdetail("Number of returned columns (%d) does not match "
                           "expected column count (%d).",
                           nincols, noutcols)));

    /*
     * Check to see if the map is one-to-one, in which case we need not do a
     * tuple conversion.  We must also insist that both tupdescs either
     * specify or don't specify an OID column, else we need a conversion to
     * add/remove space for that.  (For some callers, presence or absence of
     * an OID column perhaps would not really matter, but let's be safe.)
     */
    if (indesc->natts == outdesc->natts &&
        indesc->tdhasoid == outdesc->tdhasoid)
    {
        for (i = 0; i < n; i++)
        {
            if (attrMap[i] == (i + 1))
            {    
                continue;
            }

            /*
             * If it's a dropped column and the corresponding input column is
             * also dropped, we needn't convert.  However, attlen and attalign
             * must agree.
             */
            if (attrMap[i] == 0 &&
                indesc->attrs[i]->attisdropped &&
                indesc->attrs[i]->attlen == outdesc->attrs[i]->attlen &&
                indesc->attrs[i]->attalign == outdesc->attrs[i]->attalign)
            {  
                continue;
            }
            same = false;
            break;
        }
    }
    else
    {
        same = false;
    }

    if (same)
    {
        /* Runtime conversion is not needed */
        pfree(attrMap);
        return NULL;
    }

    /* Prepare the map structure */
    map = (TupleConversionMap *) palloc(sizeof(TupleConversionMap));
    map->attrMap = attrMap;
    map->indesc = indesc;
    map->outdesc = outdesc;
    /* preallocate workspace for Datum arrays */
    map->outisnull = (bool *) palloc(n * sizeof(bool));
    map->outvalues = (Datum *) palloc(n * sizeof(Datum));
    n = indesc->natts + 1;        /* +1 for NULL */
    map->invalues = (Datum *) palloc(n * sizeof(Datum));
    map->inisnull = (bool *) palloc(n * sizeof(bool));
    map->inisnull[0] = true;
    map->invalues[0] = (Datum) 0;    /* set up the NULL entry */

    return map;
}

/*
 * Set up for tuple conversion, matching input and output columns by name.
 * (Dropped columns are ignored in both input and output.)    This is intended
 * for use when the rowtypes are related by inheritance, so we expect an exact
 * match of both type and typmod.  The error messages will be a bit unhelpful
 * unless both rowtypes are named composite types.
 */
TupleConversionMap *
convert_tuples_by_name(TupleDesc indesc,
                       TupleDesc outdesc,
                       const char *msg)
{// #lizard forgives
    TupleConversionMap *map;
    AttrNumber *attrMap;
    int            n = outdesc->natts;
    int            i;
    bool        same;

    /* Verify compatibility and prepare attribute-number map */
    attrMap = convert_tuples_by_name_map(indesc, outdesc, msg);

    /*
     * Check to see if the map is one-to-one, in which case we need not do a
     * tuple conversion.  We must also insist that both tupdescs either
     * specify or don't specify an OID column, else we need a conversion to
     * add/remove space for that.  (For some callers, presence or absence of
     * an OID column perhaps would not really matter, but let's be safe.)
     */
    if (indesc->natts == outdesc->natts &&
        indesc->tdhasoid == outdesc->tdhasoid)
    {
        same = true;
        for (i = 0; i < n; i++)
        {
            if (attrMap[i] == (i + 1))
                continue;

            /*
             * If it's a dropped column and the corresponding input column is
             * also dropped, we needn't convert.  However, attlen and attalign
             * must agree.
             */
            if (attrMap[i] == 0 &&
                indesc->attrs[i]->attisdropped &&
                indesc->attrs[i]->attlen == outdesc->attrs[i]->attlen &&
                indesc->attrs[i]->attalign == outdesc->attrs[i]->attalign)
                continue;

            same = false;
            break;
        }
    }
    else
        same = false;

    if (same)
    {
        /* Runtime conversion is not needed */
        pfree(attrMap);
        return NULL;
    }

    /* Prepare the map structure */
    map = (TupleConversionMap *) palloc(sizeof(TupleConversionMap));
    map->indesc = indesc;
    map->outdesc = outdesc;
    map->attrMap = attrMap;
    /* preallocate workspace for Datum arrays */
    map->outvalues = (Datum *) palloc(n * sizeof(Datum));
    map->outisnull = (bool *) palloc(n * sizeof(bool));
    n = indesc->natts + 1;        /* +1 for NULL */
    map->invalues = (Datum *) palloc(n * sizeof(Datum));
    map->inisnull = (bool *) palloc(n * sizeof(bool));
    map->invalues[0] = (Datum) 0;    /* set up the NULL entry */
    map->inisnull[0] = true;

    return map;
}

/*
 * Return a palloc'd bare attribute map for tuple conversion, matching input
 * and output columns by name.  (Dropped columns are ignored in both input and
 * output.)  This is normally a subroutine for convert_tuples_by_name, but can
 * be used standalone.
 */
AttrNumber *
convert_tuples_by_name_map(TupleDesc indesc,
                           TupleDesc outdesc,
                           const char *msg)
{// #lizard forgives
    AttrNumber *attrMap;
    int         outnatts;
    int         innatts;
    int            i;
	int         nextindesc = -1;

    outnatts = outdesc->natts;
    innatts = indesc->natts;

    attrMap = (AttrNumber *) palloc0(outnatts * sizeof(AttrNumber));
    for (i = 0; i < outnatts; i++)
    {
		Form_pg_attribute outatt = TupleDescAttr(outdesc, i);
        char       *attname;
        Oid            atttypid;
        int32        atttypmod;
        int            j;

		if (outatt->attisdropped)
            continue;            /* attrMap[i] is already 0 */
		attname = NameStr(outatt->attname);
		atttypid = outatt->atttypid;
		atttypmod = outatt->atttypmod;

		/*
		* Now search for an attribute with the same name in the indesc. It
		* seems likely that a partitioned table will have the attributes in
		* the same order as the partition, so the search below is optimized
		* for that case.  It is possible that columns are dropped in one of
		* the relations, but not the other, so we use the 'nextindesc'
		* counter to track the starting point of the search.  If the inner
		* loop encounters dropped columns then it will have to skip over
		* them, but it should leave 'nextindesc' at the correct position for
		* the next outer loop.
		*/
		for (j = 0; j < innatts; j++)
        {
			Form_pg_attribute inatt;

			nextindesc++;
			if (nextindesc >= innatts)
			   nextindesc = 0;

			inatt = TupleDescAttr(indesc, nextindesc);
			if (inatt->attisdropped)
                continue;
			if (strcmp(attname, NameStr(inatt->attname)) == 0)
            {
                /* Found it, check type */
				if (atttypid != inatt->atttypid || atttypmod != inatt->atttypmod)
                    ereport(ERROR,
                            (errcode(ERRCODE_DATATYPE_MISMATCH),
                             errmsg_internal("%s", _(msg)),
                             errdetail("Attribute \"%s\" of type %s does not match corresponding attribute of type %s.",
                                       attname,
                                       format_type_be(outdesc->tdtypeid),
                                       format_type_be(indesc->tdtypeid))));
				attrMap[i] = inatt->attnum;
                break;
            }
        }
        if (attrMap[i] == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg_internal("%s", _(msg)),
                     errdetail("Attribute \"%s\" of type %s does not exist in type %s.",
                               attname,
                               format_type_be(outdesc->tdtypeid),
                               format_type_be(indesc->tdtypeid))));
    }
    return attrMap;
}

/*
 * Perform conversion of a tuple according to the map.
 */
HeapTuple
do_convert_tuple(HeapTuple tuple, TupleConversionMap *map, Relation rel)
{
    AttrNumber *attrMap = map->attrMap;
    Datum       *invalues = map->invalues;
    bool       *inisnull = map->inisnull;
    Datum       *outvalues = map->outvalues;
    bool       *outisnull = map->outisnull;
    int            outnatts = map->outdesc->natts;
    int            i;
    bool        hasshard = false;
    AttrNumber  diskey = InvalidAttrNumber;
    AttrNumber  secdiskey = InvalidAttrNumber;

    /*
     * Extract all the values of the old tuple, offsetting the arrays so that
     * invalues[0] is left NULL and invalues[1] is the first source attribute;
     * this exactly matches the numbering convention in attrMap.
     */
    heap_deform_tuple(tuple, map->indesc, invalues + 1, inisnull + 1);

    /*
     * Transpose into proper fields of the new tuple.
     */
    for (i = 0; i < outnatts; i++)
    {
        int            j = attrMap[i];

        outvalues[i] = invalues[j];
        outisnull[i] = inisnull[j];
    }

    if (rel)
    {
        hasshard = RelationIsSharded(rel);
        
        if(hasshard)
        {
            diskey = RelationGetDisKey(rel);
            secdiskey = RelationGetSecDisKey(rel);
        }
    }

    /*
     * Now form the new tuple.
     */
    if (hasshard)
    {
        return heap_form_tuple_plain(map->outdesc,
                           outvalues,
                           outisnull,
                           diskey, secdiskey, RelationGetRelid(rel));
    }
    else
        return heap_form_tuple(map->outdesc, outvalues, outisnull);
}

/*
 * Free a TupleConversionMap structure.
 */
void
free_conversion_map(TupleConversionMap *map)
{
    /* indesc and outdesc are not ours to free */
    pfree(map->attrMap);
    pfree(map->invalues);
    pfree(map->inisnull);
    pfree(map->outvalues);
    pfree(map->outisnull);
    pfree(map);
}
