/*-------------------------------------------------------------------------
 *
 * operatorcmds.c
 *
 *	  Routines for operator manipulation commands
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/operatorcmds.c,v 1.1 2002/04/15 05:22:03 tgl Exp $
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send procedures
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *		Most of the parse-tree manipulation routines are defined in
 *		commands/manip.c.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/syscache.h"


/*
 * DefineOperator
 *		this function extracts all the information from the
 *		parameter list generated by the parser and then has
 *		OperatorCreate() do all the actual work.
 *
 * 'parameters' is a list of DefElem
 */
void
DefineOperator(List *names, List *parameters)
{
	char	   *oprName;
	Oid			oprNamespace;
	uint16		precedence = 0; /* operator precedence */
	bool		canHash = false;	/* operator hashes */
	bool		isLeftAssociative = true;		/* operator is left
												 * associative */
	char	   *functionName = NULL;	/* function for operator */
	TypeName   *typeName1 = NULL;		/* first type name */
	TypeName   *typeName2 = NULL;		/* second type name */
	Oid			typeId1 = InvalidOid;	/* types converted to OID */
	Oid			typeId2 = InvalidOid;
	char	   *commutatorName = NULL;	/* optional commutator operator
										 * name */
	char	   *negatorName = NULL;		/* optional negator operator name */
	char	   *restrictionName = NULL; /* optional restrict. sel.
										 * procedure */
	char	   *joinName = NULL;	/* optional join sel. procedure name */
	char	   *sortName1 = NULL;		/* optional first sort operator */
	char	   *sortName2 = NULL;		/* optional second sort operator */
	List	   *pl;

	/* Convert list of names to a name and namespace */
	oprNamespace = QualifiedNameGetCreationNamespace(names, &oprName);

	/*
	 * loop over the definition list and extract the information we need.
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcasecmp(defel->defname, "leftarg") == 0)
		{
			typeName1 = defGetTypeName(defel);
			if (typeName1->setof)
				elog(ERROR, "setof type not implemented for leftarg");
		}
		else if (strcasecmp(defel->defname, "rightarg") == 0)
		{
			typeName2 = defGetTypeName(defel);
			if (typeName2->setof)
				elog(ERROR, "setof type not implemented for rightarg");
		}
		else if (strcasecmp(defel->defname, "procedure") == 0)
			functionName = defGetString(defel);
		else if (strcasecmp(defel->defname, "precedence") == 0)
		{
			/* NOT IMPLEMENTED (never worked in v4.2) */
			elog(NOTICE, "CREATE OPERATOR: precedence not implemented");
		}
		else if (strcasecmp(defel->defname, "associativity") == 0)
		{
			/* NOT IMPLEMENTED (never worked in v4.2) */
			elog(NOTICE, "CREATE OPERATOR: associativity not implemented");
		}
		else if (strcasecmp(defel->defname, "commutator") == 0)
			commutatorName = defGetString(defel);
		else if (strcasecmp(defel->defname, "negator") == 0)
			negatorName = defGetString(defel);
		else if (strcasecmp(defel->defname, "restrict") == 0)
			restrictionName = defGetString(defel);
		else if (strcasecmp(defel->defname, "join") == 0)
			joinName = defGetString(defel);
		else if (strcasecmp(defel->defname, "hashes") == 0)
			canHash = TRUE;
		else if (strcasecmp(defel->defname, "sort1") == 0)
			sortName1 = defGetString(defel);
		else if (strcasecmp(defel->defname, "sort2") == 0)
			sortName2 = defGetString(defel);
		else
		{
			elog(WARNING, "DefineOperator: attribute \"%s\" not recognized",
				 defel->defname);
		}
	}

	/*
	 * make sure we have our required definitions
	 */
	if (functionName == NULL)
		elog(ERROR, "Define: \"procedure\" unspecified");

	/* Transform type names to type OIDs */
	if (typeName1)
		typeId1 = typenameTypeId(typeName1);
	if (typeName2)
		typeId2 = typenameTypeId(typeName2);

	/*
	 * now have OperatorCreate do all the work..
	 */
	OperatorCreate(oprName,		/* operator name */
				   typeId1,		/* left type id */
				   typeId2,		/* right type id */
				   functionName,	/* function for operator */
				   precedence,	/* operator precedence */
				   isLeftAssociative,	/* operator is left associative */
				   commutatorName,		/* optional commutator operator
										 * name */
				   negatorName, /* optional negator operator name */
				   restrictionName,		/* optional restrict. sel.
										 * procedure */
				   joinName,	/* optional join sel. procedure name */
				   canHash,		/* operator hashes */
				   sortName1,	/* optional first sort operator */
				   sortName2);	/* optional second sort operator */

}


/*
 * RemoveOperator
 *		Deletes an operator.
 *
 * Exceptions:
 *		BadArg if name is invalid.
 *		BadArg if type1 is invalid.
 *		"ERROR" if operator nonexistent.
 *		...
 */
void
RemoveOperator(char *operatorName,		/* operator name */
			   TypeName *typeName1, /* left argument type name */
			   TypeName *typeName2) /* right argument type name */
{
	Relation	relation;
	HeapTuple	tup;
	Oid			typeId1 = InvalidOid;
	Oid			typeId2 = InvalidOid;
	char		oprtype;

	if (typeName1)
		typeId1 = typenameTypeId(typeName1);

	if (typeName2)
		typeId2 = typenameTypeId(typeName2);

	if (OidIsValid(typeId1) && OidIsValid(typeId2))
		oprtype = 'b';
	else if (OidIsValid(typeId1))
		oprtype = 'r';
	else
		oprtype = 'l';

	relation = heap_openr(OperatorRelationName, RowExclusiveLock);

	tup = SearchSysCacheCopy(OPERNAME,
							 PointerGetDatum(operatorName),
							 ObjectIdGetDatum(typeId1),
							 ObjectIdGetDatum(typeId2),
							 CharGetDatum(oprtype));

	if (HeapTupleIsValid(tup))
	{
		if (!pg_oper_ownercheck(tup->t_data->t_oid, GetUserId()))
			elog(ERROR, "RemoveOperator: operator '%s': permission denied",
				 operatorName);

		/* Delete any comments associated with this operator */
		DeleteComments(tup->t_data->t_oid, RelationGetRelid(relation));

		simple_heap_delete(relation, &tup->t_self);
	}
	else
	{
		if (OidIsValid(typeId1) && OidIsValid(typeId2))
		{
			elog(ERROR, "RemoveOperator: binary operator '%s' taking '%s' and '%s' does not exist",
				 operatorName,
				 TypeNameToString(typeName1),
				 TypeNameToString(typeName2));
		}
		else if (OidIsValid(typeId1))
		{
			elog(ERROR, "RemoveOperator: right unary operator '%s' taking '%s' does not exist",
				 operatorName,
				 TypeNameToString(typeName1));
		}
		else
		{
			elog(ERROR, "RemoveOperator: left unary operator '%s' taking '%s' does not exist",
				 operatorName,
				 TypeNameToString(typeName2));
		}
	}
	heap_freetuple(tup);
	heap_close(relation, RowExclusiveLock);
}
