/*-------------------------------------------------------------------------
 *
 * execQual.c
 *	  Routines to evaluate qualification and targetlist expressions
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execQual.c,v 1.115 2002/12/06 03:42:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecEvalExpr	- evaluate an expression and return a datum
 *		ExecEvalExprSwitchContext - same, but switch into eval memory context
 *		ExecQual		- return true/false if qualification is satisfied
 *		ExecProject		- form a new tuple by projecting the given tuple
 *
 *	 NOTES
 *		ExecEvalExpr() and ExecEvalVar() are hotspots.	making these faster
 *		will speed up the entire system.  Unfortunately they are currently
 *		implemented recursively.  Eliminating the recursion is bound to
 *		improve the speed of the executor.
 *
 *		ExecProject() is used to make tuple projections.  Rather then
 *		trying to speed it up, the execution plan should be pre-processed
 *		to facilitate attribute sharing between nodes wherever possible,
 *		instead of doing needless copying.	-cim 5/31/91
 *
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "executor/execdebug.h"
#include "executor/functions.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "parser/parse_expr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fcache.h"
#include "utils/lsyscache.h"


/* static function decls */
static Datum ExecEvalAggref(Aggref *aggref, ExprContext *econtext,
			   bool *isNull);
static Datum ExecEvalArrayRef(ArrayRef *arrayRef, ExprContext *econtext,
				 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalVar(Var *variable, ExprContext *econtext, bool *isNull);
static Datum ExecEvalOper(Expr *opClause, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalDistinct(Expr *opClause, ExprContext *econtext,
				 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalFunc(Expr *funcClause, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone);
static ExprDoneCond ExecEvalFuncArgs(FunctionCallInfo fcinfo,
				 List *argList, ExprContext *econtext);
static Datum ExecEvalNot(Expr *notclause, ExprContext *econtext, bool *isNull);
static Datum ExecEvalAnd(Expr *andExpr, ExprContext *econtext, bool *isNull);
static Datum ExecEvalOr(Expr *orExpr, ExprContext *econtext, bool *isNull);
static Datum ExecEvalCase(CaseExpr *caseExpr, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalNullTest(NullTest *ntest, ExprContext *econtext,
				 bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalBooleanTest(BooleanTest *btest, ExprContext *econtext,
					bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalConstraintTest(ConstraintTest *constraint,
					   ExprContext *econtext,
					   bool *isNull, ExprDoneCond *isDone);
static Datum ExecEvalConstraintTestValue(ConstraintTestValue *conVal,
					   ExprContext *econtext,
					   bool *isNull, ExprDoneCond *isDone);


/*----------
 *	  ExecEvalArrayRef
 *
 *	   This function takes an ArrayRef and returns the extracted Datum
 *	   if it's a simple reference, or the modified array value if it's
 *	   an array assignment (i.e., array element or slice insertion).
 *
 * NOTE: if we get a NULL result from a subexpression, we return NULL when
 * it's an array reference, or the unmodified source array when it's an
 * array assignment.  This may seem peculiar, but if we return NULL (as was
 * done in versions up through 7.0) then an assignment like
 *			UPDATE table SET arrayfield[4] = NULL
 * will result in setting the whole array to NULL, which is certainly not
 * very desirable.	By returning the source array we make the assignment
 * into a no-op, instead.  (Eventually we need to redesign arrays so that
 * individual elements can be NULL, but for now, let's try to protect users
 * from shooting themselves in the foot.)
 *
 * NOTE: we deliberately refrain from applying DatumGetArrayTypeP() here,
 * even though that might seem natural, because this code needs to support
 * both varlena arrays and fixed-length array types.  DatumGetArrayTypeP()
 * only works for the varlena kind.  The routines we call in arrayfuncs.c
 * have to know the difference (that's what they need refattrlength for).
 *----------
 */
static Datum
ExecEvalArrayRef(ArrayRef *arrayRef,
				 ExprContext *econtext,
				 bool *isNull,
				 ExprDoneCond *isDone)
{
	ArrayType  *array_source;
	ArrayType  *resultArray;
	bool		isAssignment = (arrayRef->refassgnexpr != NULL);
	List	   *elt;
	int			i = 0,
				j = 0;
	IntArray	upper,
				lower;
	int		   *lIndex;

	if (arrayRef->refexpr != NULL)
	{
		array_source = (ArrayType *)
			DatumGetPointer(ExecEvalExpr(arrayRef->refexpr,
										 econtext,
										 isNull,
										 isDone));

		/*
		 * If refexpr yields NULL, result is always NULL, for now anyway.
		 * (This means you cannot assign to an element or slice of an
		 * array that's NULL; it'll just stay NULL.)
		 */
		if (*isNull)
			return (Datum) NULL;
	}
	else
	{
		/*
		 * Empty refexpr indicates we are doing an INSERT into an array
		 * column. For now, we just take the refassgnexpr (which the
		 * parser will have ensured is an array value) and return it
		 * as-is, ignoring any subscripts that may have been supplied in
		 * the INSERT column list. This is a kluge, but it's not real
		 * clear what the semantics ought to be...
		 */
		array_source = NULL;
	}

	foreach(elt, arrayRef->refupperindexpr)
	{
		if (i >= MAXDIM)
			elog(ERROR, "ExecEvalArrayRef: can only handle %d dimensions",
				 MAXDIM);

		upper.indx[i++] = DatumGetInt32(ExecEvalExpr((Node *) lfirst(elt),
													 econtext,
													 isNull,
													 NULL));
		/* If any index expr yields NULL, result is NULL or source array */
		if (*isNull)
		{
			if (!isAssignment || array_source == NULL)
				return (Datum) NULL;
			*isNull = false;
			return PointerGetDatum(array_source);
		}
	}

	if (arrayRef->reflowerindexpr != NIL)
	{
		foreach(elt, arrayRef->reflowerindexpr)
		{
			if (j >= MAXDIM)
				elog(ERROR, "ExecEvalArrayRef: can only handle %d dimensions",
					 MAXDIM);

			lower.indx[j++] = DatumGetInt32(ExecEvalExpr((Node *) lfirst(elt),
														 econtext,
														 isNull,
														 NULL));

			/*
			 * If any index expr yields NULL, result is NULL or source
			 * array
			 */
			if (*isNull)
			{
				if (!isAssignment || array_source == NULL)
					return (Datum) NULL;
				*isNull = false;
				return PointerGetDatum(array_source);
			}
		}
		if (i != j)
			elog(ERROR,
				 "ExecEvalArrayRef: upper and lower indices mismatch");
		lIndex = lower.indx;
	}
	else
		lIndex = NULL;

	if (isAssignment)
	{
		Datum		sourceData = ExecEvalExpr(arrayRef->refassgnexpr,
											  econtext,
											  isNull,
											  NULL);

		/*
		 * For now, can't cope with inserting NULL into an array, so make
		 * it a no-op per discussion above...
		 */
		if (*isNull)
		{
			if (array_source == NULL)
				return (Datum) NULL;
			*isNull = false;
			return PointerGetDatum(array_source);
		}

		if (array_source == NULL)
			return sourceData;	/* XXX do something else? */

		if (lIndex == NULL)
			resultArray = array_set(array_source, i,
									upper.indx,
									sourceData,
									arrayRef->refattrlength,
									arrayRef->refelemlength,
									arrayRef->refelembyval,
									arrayRef->refelemalign,
									isNull);
		else
			resultArray = array_set_slice(array_source, i,
										  upper.indx, lower.indx,
							   (ArrayType *) DatumGetPointer(sourceData),
										  arrayRef->refattrlength,
										  arrayRef->refelemlength,
										  arrayRef->refelembyval,
										  arrayRef->refelemalign,
										  isNull);
		return PointerGetDatum(resultArray);
	}

	if (lIndex == NULL)
		return array_ref(array_source, i, upper.indx,
						 arrayRef->refattrlength,
						 arrayRef->refelemlength,
						 arrayRef->refelembyval,
						 arrayRef->refelemalign,
						 isNull);
	else
	{
		resultArray = array_get_slice(array_source, i,
									  upper.indx, lower.indx,
									  arrayRef->refattrlength,
									  arrayRef->refelemlength,
									  arrayRef->refelembyval,
									  arrayRef->refelemalign,
									  isNull);
		return PointerGetDatum(resultArray);
	}
}


/* ----------------------------------------------------------------
 *		ExecEvalAggref
 *
 *		Returns a Datum whose value is the value of the precomputed
 *		aggregate found in the given expression context.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalAggref(Aggref *aggref, ExprContext *econtext, bool *isNull)
{
	if (econtext->ecxt_aggvalues == NULL)		/* safety check */
		elog(ERROR, "ExecEvalAggref: no aggregates in this expression context");

	*isNull = econtext->ecxt_aggnulls[aggref->aggno];
	return econtext->ecxt_aggvalues[aggref->aggno];
}

/* ----------------------------------------------------------------
 *		ExecEvalVar
 *
 *		Returns a Datum whose value is the value of a range
 *		variable with respect to given expression context.
 *
 *
 *		As an entry condition, we expect that the datatype the
 *		plan expects to get (as told by our "variable" argument) is in
 *		fact the datatype of the attribute the plan says to fetch (as
 *		seen in the current context, identified by our "econtext"
 *		argument).
 *
 *		If we fetch a Type A attribute and Caller treats it as if it
 *		were Type B, there will be undefined results (e.g. crash).
 *		One way these might mismatch now is that we're accessing a
 *		catalog class and the type information in the pg_attribute
 *		class does not match the hardcoded pg_attribute information
 *		(in pg_attribute.h) for the class in question.
 *
 *		We have an Assert to make sure this entry condition is met.
 *
 * ---------------------------------------------------------------- */
static Datum
ExecEvalVar(Var *variable, ExprContext *econtext, bool *isNull)
{
	Datum		result;
	TupleTableSlot *slot;
	AttrNumber	attnum;
	HeapTuple	heapTuple;
	TupleDesc	tuple_type;

	/*
	 * get the slot we want
	 */
	switch (variable->varno)
	{
		case INNER:				/* get the tuple from the inner node */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER:				/* get the tuple from the outer node */
			slot = econtext->ecxt_outertuple;
			break;

		default:				/* get the tuple from the relation being
								 * scanned */
			slot = econtext->ecxt_scantuple;
			break;
	}

	/*
	 * extract tuple information from the slot
	 */
	heapTuple = slot->val;
	tuple_type = slot->ttc_tupleDescriptor;

	attnum = variable->varattno;

	/* (See prolog for explanation of this Assert) */
	Assert(attnum <= 0 ||
		   (attnum - 1 <= tuple_type->natts - 1 &&
			tuple_type->attrs[attnum - 1] != NULL &&
		  variable->vartype == tuple_type->attrs[attnum - 1]->atttypid));

	/*
	 * If the attribute number is invalid, then we are supposed to return
	 * the entire tuple; we give back a whole slot so that callers know
	 * what the tuple looks like.
	 *
	 * XXX this is a horrid crock: since the pointer to the slot might live
	 * longer than the current evaluation context, we are forced to copy
	 * the tuple and slot into a long-lived context --- we use
	 * TransactionCommandContext which should be safe enough.  This
	 * represents a serious memory leak if many such tuples are processed
	 * in one command, however.  We ought to redesign the representation
	 * of whole-tuple datums so that this is not necessary.
	 *
	 * We assume it's OK to point to the existing tupleDescriptor, rather
	 * than copy that too.
	 */
	if (attnum == InvalidAttrNumber)
	{
		MemoryContext oldContext;
		TupleTableSlot *tempSlot;
		HeapTuple	tup;

		oldContext = MemoryContextSwitchTo(TransactionCommandContext);
		tempSlot = MakeTupleTableSlot();
		tup = heap_copytuple(heapTuple);
		ExecStoreTuple(tup, tempSlot, InvalidBuffer, true);
		ExecSetSlotDescriptor(tempSlot, tuple_type, false);
		MemoryContextSwitchTo(oldContext);
		return PointerGetDatum(tempSlot);
	}

	result = heap_getattr(heapTuple,	/* tuple containing attribute */
						  attnum,		/* attribute number of desired
										 * attribute */
						  tuple_type,	/* tuple descriptor of tuple */
						  isNull);		/* return: is attribute null? */

	return result;
}

/* ----------------------------------------------------------------
 *		ExecEvalParam
 *
 *		Returns the value of a parameter.  A param node contains
 *		something like ($.name) and the expression context contains
 *		the current parameter bindings (name = "sam") (age = 34)...
 *		so our job is to find and return the appropriate datum ("sam").
 *
 *		Q: if we have a parameter ($.foo) without a binding, i.e.
 *		   there is no (foo = xxx) in the parameter list info,
 *		   is this a fatal error or should this be a "not available"
 *		   (in which case we could return NULL)?	-cim 10/13/89
 * ----------------------------------------------------------------
 */
Datum
ExecEvalParam(Param *expression, ExprContext *econtext, bool *isNull)
{
	int			thisParamKind = expression->paramkind;
	AttrNumber	thisParamId = expression->paramid;

	if (thisParamKind == PARAM_EXEC)
	{
		/*
		 * PARAM_EXEC params (internal executor parameters) are stored in
		 * the ecxt_param_exec_vals array, and can be accessed by array index.
		 */
		ParamExecData *prm;

		prm = &(econtext->ecxt_param_exec_vals[thisParamId]);
		if (prm->execPlan != NULL)
		{
			/* Parameter not evaluated yet, so go do it */
			ExecSetParamPlan(prm->execPlan, econtext);
			/* ExecSetParamPlan should have processed this param... */
			Assert(prm->execPlan == NULL);
		}
		*isNull = prm->isnull;
		return prm->value;
	}
	else
	{
		/*
		 * All other parameter types must be sought in ecxt_param_list_info.
		 * NOTE: The last entry in the param array is always an
		 * entry with kind == PARAM_INVALID.
		 */
		ParamListInfo paramList = econtext->ecxt_param_list_info;
		char	   *thisParamName = expression->paramname;
		bool		matchFound = false;

		if (paramList != NULL)
		{
			while (paramList->kind != PARAM_INVALID && !matchFound)
			{
				if (thisParamKind == paramList->kind)
				{
					switch (thisParamKind)
					{
						case PARAM_NAMED:
							if (strcmp(paramList->name, thisParamName) == 0)
								matchFound = true;
							break;
						case PARAM_NUM:
							if (paramList->id == thisParamId)
								matchFound = true;
							break;
						default:
							elog(ERROR, "ExecEvalParam: invalid paramkind %d",
								 thisParamKind);
					}
				}
				if (!matchFound)
					paramList++;
			} /* while */
		} /* if */

		if (!matchFound)
		{
			if (thisParamKind == PARAM_NAMED)
				elog(ERROR, "ExecEvalParam: Unknown value for parameter %s",
					 thisParamName);
			else
				elog(ERROR, "ExecEvalParam: Unknown value for parameter %d",
					 thisParamId);
		}

		*isNull = paramList->isnull;
		return paramList->value;
	}
}


/* ----------------------------------------------------------------
 *		ExecEvalOper / ExecEvalFunc support routines
 * ----------------------------------------------------------------
 */

/*
 *		GetAttributeByName
 *		GetAttributeByNum
 *
 *		These are functions which return the value of the
 *		named attribute out of the tuple from the arg slot.  User defined
 *		C functions which take a tuple as an argument are expected
 *		to use this.  Ex: overpaid(EMP) might call GetAttributeByNum().
 */
Datum
GetAttributeByNum(TupleTableSlot *slot,
				  AttrNumber attrno,
				  bool *isNull)
{
	Datum		retval;

	if (!AttributeNumberIsValid(attrno))
		elog(ERROR, "GetAttributeByNum: Invalid attribute number");

	if (!AttrNumberIsForUserDefinedAttr(attrno))
		elog(ERROR, "GetAttributeByNum: cannot access system attributes here");

	if (isNull == (bool *) NULL)
		elog(ERROR, "GetAttributeByNum: a NULL isNull flag was passed");

	if (TupIsNull(slot))
	{
		*isNull = true;
		return (Datum) 0;
	}

	retval = heap_getattr(slot->val,
						  attrno,
						  slot->ttc_tupleDescriptor,
						  isNull);
	if (*isNull)
		return (Datum) 0;

	return retval;
}

Datum
GetAttributeByName(TupleTableSlot *slot, char *attname, bool *isNull)
{
	AttrNumber	attrno;
	TupleDesc	tupdesc;
	Datum		retval;
	int			natts;
	int			i;

	if (attname == NULL)
		elog(ERROR, "GetAttributeByName: Invalid attribute name");

	if (isNull == (bool *) NULL)
		elog(ERROR, "GetAttributeByName: a NULL isNull flag was passed");

	if (TupIsNull(slot))
	{
		*isNull = true;
		return (Datum) 0;
	}

	tupdesc = slot->ttc_tupleDescriptor;
	natts = slot->val->t_data->t_natts;

	attrno = InvalidAttrNumber;
	for (i = 0; i < tupdesc->natts; i++)
	{
		if (namestrcmp(&(tupdesc->attrs[i]->attname), attname) == 0)
		{
			attrno = tupdesc->attrs[i]->attnum;
			break;
		}
	}

	if (attrno == InvalidAttrNumber)
		elog(ERROR, "GetAttributeByName: attribute %s not found", attname);

	retval = heap_getattr(slot->val,
						  attrno,
						  tupdesc,
						  isNull);
	if (*isNull)
		return (Datum) 0;

	return retval;
}

/*
 * Evaluate arguments for a function.
 */
static ExprDoneCond
ExecEvalFuncArgs(FunctionCallInfo fcinfo,
				 List *argList,
				 ExprContext *econtext)
{
	ExprDoneCond argIsDone;
	int			i;
	List	   *arg;

	argIsDone = ExprSingleResult;		/* default assumption */

	i = 0;
	foreach(arg, argList)
	{
		ExprDoneCond thisArgIsDone;

		fcinfo->arg[i] = ExecEvalExpr((Node *) lfirst(arg),
									  econtext,
									  &fcinfo->argnull[i],
									  &thisArgIsDone);

		if (thisArgIsDone != ExprSingleResult)
		{
			/*
			 * We allow only one argument to have a set value; we'd need
			 * much more complexity to keep track of multiple set
			 * arguments (cf. ExecTargetList) and it doesn't seem worth
			 * it.
			 */
			if (argIsDone != ExprSingleResult)
				elog(ERROR, "Functions and operators can take only one set argument");
			argIsDone = thisArgIsDone;
		}
		i++;
	}

	fcinfo->nargs = i;

	return argIsDone;
}

/*
 *		ExecMakeFunctionResult
 *
 * Evaluate the arguments to a function and then the function itself.
 */
Datum
ExecMakeFunctionResult(FunctionCachePtr fcache,
					   List *arguments,
					   ExprContext *econtext,
					   bool *isNull,
					   ExprDoneCond *isDone)
{
	Datum		result;
	FunctionCallInfoData fcinfo;
	ReturnSetInfo rsinfo;		/* for functions returning sets */
	ExprDoneCond argDone;
	bool		hasSetArg;
	int			i;

	/*
	 * arguments is a list of expressions to evaluate before passing to
	 * the function manager.  We skip the evaluation if it was already
	 * done in the previous call (ie, we are continuing the evaluation of
	 * a set-valued function).	Otherwise, collect the current argument
	 * values into fcinfo.
	 */
	if (!fcache->setArgsValid)
	{
		/* Need to prep callinfo structure */
		MemSet(&fcinfo, 0, sizeof(fcinfo));
		fcinfo.flinfo = &(fcache->func);
		argDone = ExecEvalFuncArgs(&fcinfo, arguments, econtext);
		if (argDone == ExprEndResult)
		{
			/* input is an empty set, so return an empty set. */
			*isNull = true;
			if (isDone)
				*isDone = ExprEndResult;
			else
				elog(ERROR, "Set-valued function called in context that cannot accept a set");
			return (Datum) 0;
		}
		hasSetArg = (argDone != ExprSingleResult);
	}
	else
	{
		/* Copy callinfo from previous evaluation */
		memcpy(&fcinfo, &fcache->setArgs, sizeof(fcinfo));
		hasSetArg = fcache->setHasSetArg;
		/* Reset flag (we may set it again below) */
		fcache->setArgsValid = false;
	}

	/*
	 * If function returns set, prepare a resultinfo node for
	 * communication
	 */
	if (fcache->func.fn_retset)
	{
		fcinfo.resultinfo = (Node *) &rsinfo;
		rsinfo.type = T_ReturnSetInfo;
		rsinfo.econtext = econtext;
		rsinfo.expectedDesc = NULL;
		rsinfo.allowedModes = (int) SFRM_ValuePerCall;
		rsinfo.returnMode = SFRM_ValuePerCall;
		/* isDone is filled below */
		rsinfo.setResult = NULL;
		rsinfo.setDesc = NULL;
	}

	/*
	 * now return the value gotten by calling the function manager,
	 * passing the function the evaluated parameter values.
	 */
	if (fcache->func.fn_retset || hasSetArg)
	{
		/*
		 * We need to return a set result.	Complain if caller not ready
		 * to accept one.
		 */
		if (isDone == NULL)
			elog(ERROR, "Set-valued function called in context that cannot accept a set");

		/*
		 * This loop handles the situation where we have both a set
		 * argument and a set-valued function.	Once we have exhausted the
		 * function's value(s) for a particular argument value, we have to
		 * get the next argument value and start the function over again.
		 * We might have to do it more than once, if the function produces
		 * an empty result set for a particular input value.
		 */
		for (;;)
		{
			/*
			 * If function is strict, and there are any NULL arguments,
			 * skip calling the function (at least for this set of args).
			 */
			bool		callit = true;

			if (fcache->func.fn_strict)
			{
				for (i = 0; i < fcinfo.nargs; i++)
				{
					if (fcinfo.argnull[i])
					{
						callit = false;
						break;
					}
				}
			}

			if (callit)
			{
				fcinfo.isnull = false;
				rsinfo.isDone = ExprSingleResult;
				result = FunctionCallInvoke(&fcinfo);
				*isNull = fcinfo.isnull;
				*isDone = rsinfo.isDone;
			}
			else
			{
				result = (Datum) 0;
				*isNull = true;
				*isDone = ExprEndResult;
			}

			if (*isDone != ExprEndResult)
			{
				/*
				 * Got a result from current argument.	If function itself
				 * returns set, save the current argument values to re-use
				 * on the next call.
				 */
				if (fcache->func.fn_retset)
				{
					memcpy(&fcache->setArgs, &fcinfo, sizeof(fcinfo));
					fcache->setHasSetArg = hasSetArg;
					fcache->setArgsValid = true;
				}

				/*
				 * Make sure we say we are returning a set, even if the
				 * function itself doesn't return sets.
				 */
				*isDone = ExprMultipleResult;
				break;
			}

			/* Else, done with this argument */
			if (!hasSetArg)
				break;			/* input not a set, so done */

			/* Re-eval args to get the next element of the input set */
			argDone = ExecEvalFuncArgs(&fcinfo, arguments, econtext);

			if (argDone != ExprMultipleResult)
			{
				/* End of argument set, so we're done. */
				*isNull = true;
				*isDone = ExprEndResult;
				result = (Datum) 0;
				break;
			}

			/*
			 * If we reach here, loop around to run the function on the
			 * new argument.
			 */
		}
	}
	else
	{
		/*
		 * Non-set case: much easier.
		 *
		 * If function is strict, and there are any NULL arguments, skip
		 * calling the function and return NULL.
		 */
		if (fcache->func.fn_strict)
		{
			for (i = 0; i < fcinfo.nargs; i++)
			{
				if (fcinfo.argnull[i])
				{
					*isNull = true;
					return (Datum) 0;
				}
			}
		}
		fcinfo.isnull = false;
		result = FunctionCallInvoke(&fcinfo);
		*isNull = fcinfo.isnull;
	}

	return result;
}


/*
 *		ExecMakeTableFunctionResult
 *
 * Evaluate a table function, producing a materialized result in a Tuplestore
 * object.	(If function returns an empty set, we just return NULL instead.)
 */
Tuplestorestate *
ExecMakeTableFunctionResult(Node *funcexpr,
							ExprContext *econtext,
							TupleDesc expectedDesc,
							TupleDesc *returnDesc)
{
	Tuplestorestate *tupstore = NULL;
	TupleDesc	tupdesc = NULL;
	Oid			funcrettype;
	FunctionCallInfoData fcinfo;
	ReturnSetInfo rsinfo;
	MemoryContext callerContext;
	MemoryContext oldcontext;
	TupleTableSlot *slot;
	bool		direct_function_call;
	bool		first_time = true;
	bool		returnsTuple = false;

	/*
	 * Normally the passed expression tree will be a FUNC_EXPR, since the
	 * grammar only allows a function call at the top level of a table
	 * function reference.  However, if the function doesn't return set then
	 * the planner might have replaced the function call via constant-folding
	 * or inlining.  So if we see any other kind of expression node, execute
	 * it via the general ExecEvalExpr() code; the only difference is that
	 * we don't get a chance to pass a special ReturnSetInfo to any functions
	 * buried in the expression.
	 */
	if (funcexpr &&
		IsA(funcexpr, Expr) &&
		((Expr *) funcexpr)->opType == FUNC_EXPR)
	{
		Func	   *func;
		List	   *argList;
		FunctionCachePtr fcache;
		ExprDoneCond argDone;

		/*
		 * This path is similar to ExecMakeFunctionResult.
		 */
		direct_function_call = true;

		funcrettype = ((Expr *) funcexpr)->typeOid;
		func = (Func *) ((Expr *) funcexpr)->oper;
		argList = ((Expr *) funcexpr)->args;

		/*
		 * get the fcache from the Func node. If it is NULL, then initialize
		 * it
		 */
		fcache = func->func_fcache;
		if (fcache == NULL)
		{
			fcache = init_fcache(func->funcid, length(argList),
								 econtext->ecxt_per_query_memory);
			func->func_fcache = fcache;
		}

		/*
		 * Evaluate the function's argument list.
		 *
		 * Note: ideally, we'd do this in the per-tuple context, but then the
		 * argument values would disappear when we reset the context in the
		 * inner loop.	So do it in caller context.  Perhaps we should make a
		 * separate context just to hold the evaluated arguments?
		 */
		MemSet(&fcinfo, 0, sizeof(fcinfo));
		fcinfo.flinfo = &(fcache->func);
		argDone = ExecEvalFuncArgs(&fcinfo, argList, econtext);
		/* We don't allow sets in the arguments of the table function */
		if (argDone != ExprSingleResult)
			elog(ERROR, "Set-valued function called in context that cannot accept a set");

		/*
		 * If function is strict, and there are any NULL arguments, skip
		 * calling the function and return NULL (actually an empty set).
		 */
		if (fcache->func.fn_strict)
		{
			int			i;

			for (i = 0; i < fcinfo.nargs; i++)
			{
				if (fcinfo.argnull[i])
				{
					*returnDesc = NULL;
					return NULL;
				}
			}
		}
	}
	else
	{
		/* Treat funcexpr as a generic expression */
		direct_function_call = false;
		funcrettype = exprType(funcexpr);
	}

	/*
	 * Prepare a resultinfo node for communication.  We always do this
	 * even if not expecting a set result, so that we can pass
	 * expectedDesc.  In the generic-expression case, the expression
	 * doesn't actually get to see the resultinfo, but set it up anyway
	 * because we use some of the fields as our own state variables.
	 */
	fcinfo.resultinfo = (Node *) &rsinfo;
	rsinfo.type = T_ReturnSetInfo;
	rsinfo.econtext = econtext;
	rsinfo.expectedDesc = expectedDesc;
	rsinfo.allowedModes = (int) (SFRM_ValuePerCall | SFRM_Materialize);
	rsinfo.returnMode = SFRM_ValuePerCall;
	/* isDone is filled below */
	rsinfo.setResult = NULL;
	rsinfo.setDesc = NULL;

	/*
	 * Switch to short-lived context for calling the function or expression.
	 */
	callerContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Loop to handle the ValuePerCall protocol (which is also the same
	 * behavior needed in the generic ExecEvalExpr path).
	 */
	for (;;)
	{
		Datum		result;
		HeapTuple	tuple;

		/*
		 * reset per-tuple memory context before each call of the
		 * function or expression. This cleans up any local memory the
		 * function may leak when called.
		 */
		ResetExprContext(econtext);

		/* Call the function or expression one time */
		if (direct_function_call)
		{
			fcinfo.isnull = false;
			rsinfo.isDone = ExprSingleResult;
			result = FunctionCallInvoke(&fcinfo);
		}
		else
		{
			result = ExecEvalExpr(funcexpr, econtext,
								  &fcinfo.isnull, &rsinfo.isDone);
		}

		/* Which protocol does function want to use? */
		if (rsinfo.returnMode == SFRM_ValuePerCall)
		{
			/*
			 * Check for end of result set.
			 *
			 * Note: if function returns an empty set, we don't build a
			 * tupdesc or tuplestore (since we can't get a tupdesc in the
			 * function-returning-tuple case)
			 */
			if (rsinfo.isDone == ExprEndResult)
				break;

			/*
			 * If first time through, build tupdesc and tuplestore for
			 * result
			 */
			if (first_time)
			{
				oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
				if (funcrettype == RECORDOID ||
					get_typtype(funcrettype) == 'c')
				{
					/*
					 * Composite type, so function should have returned a
					 * TupleTableSlot; use its descriptor
					 */
					slot = (TupleTableSlot *) DatumGetPointer(result);
					if (fcinfo.isnull ||
						!slot ||
						!IsA(slot, TupleTableSlot) ||
						!slot->ttc_tupleDescriptor)
						elog(ERROR, "ExecMakeTableFunctionResult: Invalid result from function returning tuple");
					tupdesc = CreateTupleDescCopy(slot->ttc_tupleDescriptor);
					returnsTuple = true;
				}
				else
				{
					/*
					 * Scalar type, so make a single-column descriptor
					 */
					tupdesc = CreateTemplateTupleDesc(1, false);
					TupleDescInitEntry(tupdesc,
									   (AttrNumber) 1,
									   "column",
									   funcrettype,
									   -1,
									   0,
									   false);
				}
				tupstore = tuplestore_begin_heap(true,	/* randomAccess */
												 SortMem);
				MemoryContextSwitchTo(oldcontext);
				rsinfo.setResult = tupstore;
				rsinfo.setDesc = tupdesc;
			}

			/*
			 * Store current resultset item.
			 */
			if (returnsTuple)
			{
				slot = (TupleTableSlot *) DatumGetPointer(result);
				if (fcinfo.isnull ||
					!slot ||
					!IsA(slot, TupleTableSlot) ||
					TupIsNull(slot))
					elog(ERROR, "ExecMakeTableFunctionResult: Invalid result from function returning tuple");
				tuple = slot->val;
			}
			else
			{
				char		nullflag;

				nullflag = fcinfo.isnull ? 'n' : ' ';
				tuple = heap_formtuple(tupdesc, &result, &nullflag);
			}

			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			tuplestore_puttuple(tupstore, tuple);
			MemoryContextSwitchTo(oldcontext);

			/*
			 * Are we done?
			 */
			if (rsinfo.isDone != ExprMultipleResult)
				break;
		}
		else if (rsinfo.returnMode == SFRM_Materialize)
		{
			/* check we're on the same page as the function author */
			if (!first_time || rsinfo.isDone != ExprSingleResult)
				elog(ERROR, "ExecMakeTableFunctionResult: Materialize-mode protocol not followed");
			/* Done evaluating the set result */
			break;
		}
		else
			elog(ERROR, "ExecMakeTableFunctionResult: unknown returnMode %d",
				 (int) rsinfo.returnMode);

		first_time = false;
	}

	/* If we have a locally-created tupstore, close it up */
	if (tupstore)
	{
		MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
		tuplestore_donestoring(tupstore);
	}

	MemoryContextSwitchTo(callerContext);

	/* The returned pointers are those in rsinfo */
	*returnDesc = rsinfo.setDesc;
	return rsinfo.setResult;
}


/* ----------------------------------------------------------------
 *		ExecEvalOper
 *		ExecEvalFunc
 *		ExecEvalDistinct
 *
 *		Evaluate the functional result of a list of arguments by calling the
 *		function manager.
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecEvalOper
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalOper(Expr *opClause,
			 ExprContext *econtext,
			 bool *isNull,
			 ExprDoneCond *isDone)
{
	Oper	   *op;
	List	   *argList;
	FunctionCachePtr fcache;

	/*
	 * we extract the oid of the function associated with the op and then
	 * pass the work onto ExecMakeFunctionResult which evaluates the
	 * arguments and returns the result of calling the function on the
	 * evaluated arguments.
	 */
	op = (Oper *) opClause->oper;
	argList = opClause->args;

	/*
	 * get the fcache from the Oper node. If it is NULL, then initialize
	 * it
	 */
	fcache = op->op_fcache;
	if (fcache == NULL)
	{
		fcache = init_fcache(op->opid, length(argList),
							 econtext->ecxt_per_query_memory);
		op->op_fcache = fcache;
	}

	return ExecMakeFunctionResult(fcache, argList, econtext,
								  isNull, isDone);
}

/* ----------------------------------------------------------------
 *		ExecEvalFunc
 * ----------------------------------------------------------------
 */

static Datum
ExecEvalFunc(Expr *funcClause,
			 ExprContext *econtext,
			 bool *isNull,
			 ExprDoneCond *isDone)
{
	Func	   *func;
	List	   *argList;
	FunctionCachePtr fcache;

	/*
	 * we extract the oid of the function associated with the func node
	 * and then pass the work onto ExecMakeFunctionResult which evaluates
	 * the arguments and returns the result of calling the function on the
	 * evaluated arguments.
	 *
	 * this is nearly identical to the ExecEvalOper code.
	 */
	func = (Func *) funcClause->oper;
	argList = funcClause->args;

	/*
	 * get the fcache from the Func node. If it is NULL, then initialize
	 * it
	 */
	fcache = func->func_fcache;
	if (fcache == NULL)
	{
		fcache = init_fcache(func->funcid, length(argList),
							 econtext->ecxt_per_query_memory);
		func->func_fcache = fcache;
	}

	return ExecMakeFunctionResult(fcache, argList, econtext,
								  isNull, isDone);
}

/* ----------------------------------------------------------------
 *		ExecEvalDistinct
 *
 * IS DISTINCT FROM must evaluate arguments to determine whether
 * they are NULL; if either is NULL then the result is already
 * known. If neither is NULL, then proceed to evaluate the
 * function. Note that this is *always* derived from the equals
 * operator, but since we need special processing of the arguments
 * we can not simply reuse ExecEvalOper() or ExecEvalFunc().
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalDistinct(Expr *opClause,
				 ExprContext *econtext,
				 bool *isNull,
				 ExprDoneCond *isDone)
{
	Datum		result;
	FunctionCachePtr fcache;
	FunctionCallInfoData fcinfo;
	ExprDoneCond argDone;
	Oper	   *op;
	List	   *argList;

	/*
	 * extract info from opClause
	 */
	op = (Oper *) opClause->oper;
	argList = opClause->args;

	/*
	 * get the fcache from the Oper node. If it is NULL, then initialize
	 * it
	 */
	fcache = op->op_fcache;
	if (fcache == NULL)
	{
		fcache = init_fcache(op->opid, length(argList),
							 econtext->ecxt_per_query_memory);
		op->op_fcache = fcache;
	}
	Assert(!fcache->func.fn_retset);

	/* Need to prep callinfo structure */
	MemSet(&fcinfo, 0, sizeof(fcinfo));
	fcinfo.flinfo = &(fcache->func);
	argDone = ExecEvalFuncArgs(&fcinfo, argList, econtext);
	if (argDone != ExprSingleResult)
		elog(ERROR, "IS DISTINCT FROM does not support set arguments");
	Assert(fcinfo.nargs == 2);

	if (fcinfo.argnull[0] && fcinfo.argnull[1])
	{
		/* Both NULL? Then is not distinct... */
		result = BoolGetDatum(FALSE);
	}
	else if (fcinfo.argnull[0] || fcinfo.argnull[1])
	{
		/* Only one is NULL? Then is distinct... */
		result = BoolGetDatum(TRUE);
	}
	else
	{
		fcinfo.isnull = false;
		result = FunctionCallInvoke(&fcinfo);
		*isNull = fcinfo.isnull;
		/* Must invert result of "=" */
		result = BoolGetDatum(!DatumGetBool(result));
	}

	return result;
}

/* ----------------------------------------------------------------
 *		ExecEvalNot
 *		ExecEvalOr
 *		ExecEvalAnd
 *
 *		Evaluate boolean expressions.  Evaluation of 'or' is
 *		short-circuited when the first true (or null) value is found.
 *
 *		The query planner reformulates clause expressions in the
 *		qualification to conjunctive normal form.  If we ever get
 *		an AND to evaluate, we can be sure that it's not a top-level
 *		clause in the qualification, but appears lower (as a function
 *		argument, for example), or in the target list.	Not that you
 *		need to know this, mind you...
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalNot(Expr *notclause, ExprContext *econtext, bool *isNull)
{
	Node	   *clause;
	Datum		expr_value;

	clause = lfirst(notclause->args);

	expr_value = ExecEvalExpr(clause, econtext, isNull, NULL);

	/*
	 * if the expression evaluates to null, then we just cascade the null
	 * back to whoever called us.
	 */
	if (*isNull)
		return expr_value;

	/*
	 * evaluation of 'not' is simple.. expr is false, then return 'true'
	 * and vice versa.
	 */
	return BoolGetDatum(!DatumGetBool(expr_value));
}

/* ----------------------------------------------------------------
 *		ExecEvalOr
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalOr(Expr *orExpr, ExprContext *econtext, bool *isNull)
{
	List	   *clauses;
	List	   *clause;
	bool		AnyNull;
	Datum		clause_value;

	clauses = orExpr->args;
	AnyNull = false;

	/*
	 * If any of the clauses is TRUE, the OR result is TRUE regardless of
	 * the states of the rest of the clauses, so we can stop evaluating
	 * and return TRUE immediately.  If none are TRUE and one or more is
	 * NULL, we return NULL; otherwise we return FALSE.  This makes sense
	 * when you interpret NULL as "don't know": if we have a TRUE then the
	 * OR is TRUE even if we aren't sure about some of the other inputs.
	 * If all the known inputs are FALSE, but we have one or more "don't
	 * knows", then we have to report that we "don't know" what the OR's
	 * result should be --- perhaps one of the "don't knows" would have
	 * been TRUE if we'd known its value.  Only when all the inputs are
	 * known to be FALSE can we state confidently that the OR's result is
	 * FALSE.
	 */
	foreach(clause, clauses)
	{
		clause_value = ExecEvalExpr((Node *) lfirst(clause),
									econtext, isNull, NULL);

		/*
		 * if we have a non-null true result, then return it.
		 */
		if (*isNull)
			AnyNull = true;		/* remember we got a null */
		else if (DatumGetBool(clause_value))
			return clause_value;
	}

	/* AnyNull is true if at least one clause evaluated to NULL */
	*isNull = AnyNull;
	return BoolGetDatum(false);
}

/* ----------------------------------------------------------------
 *		ExecEvalAnd
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalAnd(Expr *andExpr, ExprContext *econtext, bool *isNull)
{
	List	   *clauses;
	List	   *clause;
	bool		AnyNull;
	Datum		clause_value;

	clauses = andExpr->args;
	AnyNull = false;

	/*
	 * If any of the clauses is FALSE, the AND result is FALSE regardless
	 * of the states of the rest of the clauses, so we can stop evaluating
	 * and return FALSE immediately.  If none are FALSE and one or more is
	 * NULL, we return NULL; otherwise we return TRUE.	This makes sense
	 * when you interpret NULL as "don't know", using the same sort of
	 * reasoning as for OR, above.
	 */
	foreach(clause, clauses)
	{
		clause_value = ExecEvalExpr((Node *) lfirst(clause),
									econtext, isNull, NULL);

		/*
		 * if we have a non-null false result, then return it.
		 */
		if (*isNull)
			AnyNull = true;		/* remember we got a null */
		else if (!DatumGetBool(clause_value))
			return clause_value;
	}

	/* AnyNull is true if at least one clause evaluated to NULL */
	*isNull = AnyNull;
	return BoolGetDatum(!AnyNull);
}

/* ----------------------------------------------------------------
 *		ExecEvalCase
 *
 *		Evaluate a CASE clause. Will have boolean expressions
 *		inside the WHEN clauses, and will have expressions
 *		for results.
 *		- thomas 1998-11-09
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalCase(CaseExpr *caseExpr, ExprContext *econtext,
			 bool *isNull, ExprDoneCond *isDone)
{
	List	   *clauses;
	List	   *clause;
	Datum		clause_value;

	clauses = caseExpr->args;

	/*
	 * we evaluate each of the WHEN clauses in turn, as soon as one is
	 * true we return the corresponding result. If none are true then we
	 * return the value of the default clause, or NULL if there is none.
	 */
	foreach(clause, clauses)
	{
		CaseWhen   *wclause = lfirst(clause);

		clause_value = ExecEvalExpr(wclause->expr,
									econtext,
									isNull,
									NULL);

		/*
		 * if we have a true test, then we return the result, since the
		 * case statement is satisfied.  A NULL result from the test is
		 * not considered true.
		 */
		if (DatumGetBool(clause_value) && !*isNull)
		{
			return ExecEvalExpr(wclause->result,
								econtext,
								isNull,
								isDone);
		}
	}

	if (caseExpr->defresult)
	{
		return ExecEvalExpr(caseExpr->defresult,
							econtext,
							isNull,
							isDone);
	}

	*isNull = true;
	return (Datum) 0;
}

/* ----------------------------------------------------------------
 *		ExecEvalNullTest
 *
 *		Evaluate a NullTest node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalNullTest(NullTest *ntest,
				 ExprContext *econtext,
				 bool *isNull,
				 ExprDoneCond *isDone)
{
	Datum		result;

	result = ExecEvalExpr(ntest->arg, econtext, isNull, isDone);
	switch (ntest->nulltesttype)
	{
		case IS_NULL:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(true);
			}
			else
				return BoolGetDatum(false);
		case IS_NOT_NULL:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(false);
			}
			else
				return BoolGetDatum(true);
		default:
			elog(ERROR, "ExecEvalNullTest: unexpected nulltesttype %d",
				 (int) ntest->nulltesttype);
			return (Datum) 0;	/* keep compiler quiet */
	}
}

/* ----------------------------------------------------------------
 *		ExecEvalBooleanTest
 *
 *		Evaluate a BooleanTest node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalBooleanTest(BooleanTest *btest,
					ExprContext *econtext,
					bool *isNull,
					ExprDoneCond *isDone)
{
	Datum		result;

	result = ExecEvalExpr(btest->arg, econtext, isNull, isDone);
	switch (btest->booltesttype)
	{
		case IS_TRUE:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(false);
			}
			else if (DatumGetBool(result))
				return BoolGetDatum(true);
			else
				return BoolGetDatum(false);
		case IS_NOT_TRUE:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(true);
			}
			else if (DatumGetBool(result))
				return BoolGetDatum(false);
			else
				return BoolGetDatum(true);
		case IS_FALSE:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(false);
			}
			else if (DatumGetBool(result))
				return BoolGetDatum(false);
			else
				return BoolGetDatum(true);
		case IS_NOT_FALSE:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(true);
			}
			else if (DatumGetBool(result))
				return BoolGetDatum(true);
			else
				return BoolGetDatum(false);
		case IS_UNKNOWN:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(true);
			}
			else
				return BoolGetDatum(false);
		case IS_NOT_UNKNOWN:
			if (*isNull)
			{
				*isNull = false;
				return BoolGetDatum(false);
			}
			else
				return BoolGetDatum(true);
		default:
			elog(ERROR, "ExecEvalBooleanTest: unexpected booltesttype %d",
				 (int) btest->booltesttype);
			return (Datum) 0;	/* keep compiler quiet */
	}
}

/*
 * ExecEvalConstraintTestValue
 *
 * Return the value stored by constraintTest.
 */
static Datum
ExecEvalConstraintTestValue(ConstraintTestValue *conVal, ExprContext *econtext,
							bool *isNull, ExprDoneCond *isDone)
{
	/*
	 * If the Datum hasn't been set, then it's ExecEvalConstraintTest
	 * hasn't been called.
	 */
	*isNull = econtext->domainValue_isNull;
	return econtext->domainValue_datum;
}

/*
 * ExecEvalConstraintTest
 *
 * Test the constraint against the data provided.  If the data fits
 * within the constraint specifications, pass it through (return the
 * datum) otherwise throw an error.
 */
static Datum
ExecEvalConstraintTest(ConstraintTest *constraint, ExprContext *econtext,
					   bool *isNull, ExprDoneCond *isDone)
{
	Datum		result;

	result = ExecEvalExpr(constraint->arg, econtext, isNull, isDone);

	switch (constraint->testtype)
	{
		case CONSTR_TEST_NOTNULL:
			if (*isNull)
				elog(ERROR, "Domain %s does not allow NULL values",
					 constraint->domname);
			break;
		case CONSTR_TEST_CHECK:
			{
				Datum	conResult;

				/* Var with attnum == UnassignedAttrNum uses the result */
				econtext->domainValue_datum = result;
				econtext->domainValue_isNull = *isNull;

				conResult = ExecEvalExpr(constraint->check_expr, econtext, isNull, isDone);

				if (!DatumGetBool(conResult))
					elog(ERROR, "Domain %s constraint %s failed",
						 constraint->name, constraint->domname);
			}
			break;
		default:
			elog(ERROR, "ExecEvalConstraintTest: Constraint type unknown");
			break;
	}

	/* If all has gone well (constraint did not fail) return the datum */
	return result;
}

/* ----------------------------------------------------------------
 *		ExecEvalFieldSelect
 *
 *		Evaluate a FieldSelect node.
 * ----------------------------------------------------------------
 */
static Datum
ExecEvalFieldSelect(FieldSelect *fselect,
					ExprContext *econtext,
					bool *isNull,
					ExprDoneCond *isDone)
{
	Datum		result;
	TupleTableSlot *resSlot;

	result = ExecEvalExpr(fselect->arg, econtext, isNull, isDone);
	if (*isNull)
		return result;
	resSlot = (TupleTableSlot *) DatumGetPointer(result);
	Assert(resSlot != NULL && IsA(resSlot, TupleTableSlot));
	result = heap_getattr(resSlot->val,
						  fselect->fieldnum,
						  resSlot->ttc_tupleDescriptor,
						  isNull);
	return result;
}

/* ----------------------------------------------------------------
 *		ExecEvalExpr
 *
 *		Recursively evaluate a targetlist or qualification expression.
 *
 * Inputs:
 *		expression: the expression tree to evaluate
 *		econtext: evaluation context information
 *
 * Outputs:
 *		return value: Datum value of result
 *		*isNull: set to TRUE if result is NULL (actual return value is
 *				 meaningless if so); set to FALSE if non-null result
 *		*isDone: set to indicator of set-result status
 *
 * A caller that can only accept a singleton (non-set) result should pass
 * NULL for isDone; if the expression computes a set result then an elog()
 * error will be reported.	If the caller does pass an isDone pointer then
 * *isDone is set to one of these three states:
 *		ExprSingleResult		singleton result (not a set)
 *		ExprMultipleResult		return value is one element of a set
 *		ExprEndResult			there are no more elements in the set
 * When ExprMultipleResult is returned, the caller should invoke
 * ExecEvalExpr() repeatedly until ExprEndResult is returned.  ExprEndResult
 * is returned after the last real set element.  For convenience isNull will
 * always be set TRUE when ExprEndResult is returned, but this should not be
 * taken as indicating a NULL element of the set.  Note that these return
 * conventions allow us to distinguish among a singleton NULL, a NULL element
 * of a set, and an empty set.
 *
 * The caller should already have switched into the temporary memory
 * context econtext->ecxt_per_tuple_memory.  The convenience entry point
 * ExecEvalExprSwitchContext() is provided for callers who don't prefer to
 * do the switch in an outer loop.	We do not do the switch here because
 * it'd be a waste of cycles during recursive entries to ExecEvalExpr().
 *
 * This routine is an inner loop routine and must be as fast as possible.
 * ----------------------------------------------------------------
 */
Datum
ExecEvalExpr(Node *expression,
			 ExprContext *econtext,
			 bool *isNull,
			 ExprDoneCond *isDone)
{
	Datum		retDatum;

	/* Set default values for result flags: non-null, not a set result */
	*isNull = false;
	if (isDone)
		*isDone = ExprSingleResult;

	/* Is this still necessary?  Doubtful... */
	if (expression == NULL)
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * here we dispatch the work to the appropriate type of function given
	 * the type of our expression.
	 */
	switch (nodeTag(expression))
	{
		case T_Var:
			retDatum = ExecEvalVar((Var *) expression, econtext, isNull);
			break;
		case T_Const:
			{
				Const	   *con = (Const *) expression;

				retDatum = con->constvalue;
				*isNull = con->constisnull;
				break;
			}
		case T_Param:
			retDatum = ExecEvalParam((Param *) expression, econtext, isNull);
			break;
		case T_Aggref:
			retDatum = ExecEvalAggref((Aggref *) expression, econtext, isNull);
			break;
		case T_ArrayRef:
			retDatum = ExecEvalArrayRef((ArrayRef *) expression,
										econtext,
										isNull,
										isDone);
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) expression;

				switch (expr->opType)
				{
					case OP_EXPR:
						retDatum = ExecEvalOper(expr, econtext,
												isNull, isDone);
						break;
					case FUNC_EXPR:
						retDatum = ExecEvalFunc(expr, econtext,
												isNull, isDone);
						break;
					case OR_EXPR:
						retDatum = ExecEvalOr(expr, econtext, isNull);
						break;
					case AND_EXPR:
						retDatum = ExecEvalAnd(expr, econtext, isNull);
						break;
					case NOT_EXPR:
						retDatum = ExecEvalNot(expr, econtext, isNull);
						break;
					case DISTINCT_EXPR:
						retDatum = ExecEvalDistinct(expr, econtext,
													isNull, isDone);
						break;
					case SUBPLAN_EXPR:
						/* XXX temporary hack to find exec state node */
						retDatum = ExecSubPlan(((SubPlan *) expr->oper)->pstate,
											   expr->args, econtext,
											   isNull);
						break;
					default:
						elog(ERROR, "ExecEvalExpr: unknown expression type %d",
							 expr->opType);
						retDatum = 0;	/* keep compiler quiet */
						break;
				}
				break;
			}
		case T_FieldSelect:
			retDatum = ExecEvalFieldSelect((FieldSelect *) expression,
										   econtext,
										   isNull,
										   isDone);
			break;
		case T_RelabelType:
			retDatum = ExecEvalExpr(((RelabelType *) expression)->arg,
									econtext,
									isNull,
									isDone);
			break;
		case T_CaseExpr:
			retDatum = ExecEvalCase((CaseExpr *) expression,
									econtext,
									isNull,
									isDone);
			break;
		case T_NullTest:
			retDatum = ExecEvalNullTest((NullTest *) expression,
										econtext,
										isNull,
										isDone);
			break;
		case T_BooleanTest:
			retDatum = ExecEvalBooleanTest((BooleanTest *) expression,
										   econtext,
										   isNull,
										   isDone);
			break;
		case T_ConstraintTest:
			retDatum = ExecEvalConstraintTest((ConstraintTest *) expression,
											  econtext,
											  isNull,
											  isDone);
			break;
		case T_ConstraintTestValue:
			retDatum = ExecEvalConstraintTestValue((ConstraintTestValue *) expression,
												  econtext,
												  isNull,
												  isDone);
			break;
		default:
			elog(ERROR, "ExecEvalExpr: unknown expression type %d",
				 nodeTag(expression));
			retDatum = 0;		/* keep compiler quiet */
			break;
	}

	return retDatum;
}	/* ExecEvalExpr() */


/*
 * Same as above, but get into the right allocation context explicitly.
 */
Datum
ExecEvalExprSwitchContext(Node *expression,
						  ExprContext *econtext,
						  bool *isNull,
						  ExprDoneCond *isDone)
{
	Datum		retDatum;
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	retDatum = ExecEvalExpr(expression, econtext, isNull, isDone);
	MemoryContextSwitchTo(oldContext);
	return retDatum;
}


/*
 * ExecInitExpr: prepare an expression tree for execution
 *
 *	'node' is the root of the expression tree to examine
 *	'parent' is the PlanState node that owns the expression,
 *		or NULL if we are preparing an expression that is not associated
 *		with a plan.  (If so, it can't have Aggrefs or SubPlans.)
 *
 * Soon this will generate an expression state tree paralleling the given
 * expression tree.  Right now, it just searches the expression tree for
 * Aggref and SubPlan nodes.
 */
Node *
ExecInitExpr(Node *node, PlanState *parent)
{
	List	   *temp;

	if (node == NULL)
		return NULL;
	switch (nodeTag(node))
	{
		case T_Var:
			break;
		case T_Const:
			break;
		case T_Param:
			break;
		case T_Aggref:
			if (parent && IsA(parent, AggState))
			{
				AggState   *aggstate = (AggState *) parent;
				int			naggs;

				aggstate->aggs = lcons(node, aggstate->aggs);
				naggs = ++aggstate->numaggs;

				ExecInitExpr(((Aggref *) node)->target, parent);

				/*
				 * Complain if the aggregate's argument contains any
				 * aggregates; nested agg functions are semantically
				 * nonsensical.  (This probably was caught earlier,
				 * but we defend against it here anyway.)
				 */
				if (naggs != aggstate->numaggs)
					elog(ERROR, "Aggregate function calls may not be nested");
			}
			else
				elog(ERROR, "ExecInitExpr: Aggref not expected here");
			break;
		case T_ArrayRef:
			{
				ArrayRef   *aref = (ArrayRef *) node;

				ExecInitExpr((Node *) aref->refupperindexpr, parent);
				ExecInitExpr((Node *) aref->reflowerindexpr, parent);
				ExecInitExpr(aref->refexpr, parent);
				ExecInitExpr(aref->refassgnexpr, parent);
			}
			break;
		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				switch (expr->opType)
				{
					case OP_EXPR:
						break;
					case FUNC_EXPR:
						break;
					case OR_EXPR:
						break;
					case AND_EXPR:
						break;
					case NOT_EXPR:
						break;
					case DISTINCT_EXPR:
						break;
					case SUBPLAN_EXPR:
						if (parent)
						{
							SubLink *sublink = ((SubPlan *) expr->oper)->sublink;

							/*
							 * Here we just add the SubPlan nodes to
							 * parent->subPlan.  Later they will be expanded
							 * to SubPlanState nodes.
							 */
							parent->subPlan = lcons(expr->oper,
													parent->subPlan);

							/* Must recurse into oper list too */
							Assert(IsA(sublink, SubLink));
							if (sublink->lefthand)
								elog(ERROR, "ExecInitExpr: sublink has not been transformed");
							ExecInitExpr((Node *) sublink->oper, parent);
						}
						else
							elog(ERROR, "ExecInitExpr: SubPlan not expected here");
						break;
					default:
						elog(ERROR, "ExecInitExpr: unknown expression type %d",
							 expr->opType);
						break;
				}
				/* for all Expr node types, examine args list */
				ExecInitExpr((Node *) expr->args, parent);
			}
			break;
		case T_FieldSelect:
			ExecInitExpr(((FieldSelect *) node)->arg, parent);
			break;
		case T_RelabelType:
			ExecInitExpr(((RelabelType *) node)->arg, parent);
			break;
		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;

				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(temp);

					Assert(IsA(when, CaseWhen));
					ExecInitExpr(when->expr, parent);
					ExecInitExpr(when->result, parent);
				}
				/* caseexpr->arg should be null, but we'll check it anyway */
				ExecInitExpr(caseexpr->arg, parent);
				ExecInitExpr(caseexpr->defresult, parent);
			}
			break;
		case T_NullTest:
			ExecInitExpr(((NullTest *) node)->arg, parent);
			break;
		case T_BooleanTest:
			ExecInitExpr(((BooleanTest *) node)->arg, parent);
			break;
		case T_ConstraintTest:
			ExecInitExpr(((ConstraintTest *) node)->arg, parent);
			ExecInitExpr(((ConstraintTest *) node)->check_expr, parent);
			break;
		case T_ConstraintTestValue:
			break;
		case T_List:
			foreach(temp, (List *) node)
			{
				ExecInitExpr((Node *) lfirst(temp), parent);
			}
			break;
		case T_TargetEntry:
			ExecInitExpr(((TargetEntry *) node)->expr, parent);
			break;
		default:
			elog(ERROR, "ExecInitExpr: unknown expression type %d",
				 nodeTag(node));
			break;
	}

	return node;
}


/* ----------------------------------------------------------------
 *					 ExecQual / ExecTargetList / ExecProject
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecQual
 *
 *		Evaluates a conjunctive boolean expression (qual list) and
 *		returns true iff none of the subexpressions are false.
 *		(We also return true if the list is empty.)
 *
 *	If some of the subexpressions yield NULL but none yield FALSE,
 *	then the result of the conjunction is NULL (ie, unknown)
 *	according to three-valued boolean logic.  In this case,
 *	we return the value specified by the "resultForNull" parameter.
 *
 *	Callers evaluating WHERE clauses should pass resultForNull=FALSE,
 *	since SQL specifies that tuples with null WHERE results do not
 *	get selected.  On the other hand, callers evaluating constraint
 *	conditions should pass resultForNull=TRUE, since SQL also specifies
 *	that NULL constraint conditions are not failures.
 *
 *	NOTE: it would not be correct to use this routine to evaluate an
 *	AND subclause of a boolean expression; for that purpose, a NULL
 *	result must be returned as NULL so that it can be properly treated
 *	in the next higher operator (cf. ExecEvalAnd and ExecEvalOr).
 *	This routine is only used in contexts where a complete expression
 *	is being evaluated and we know that NULL can be treated the same
 *	as one boolean result or the other.
 *
 * ----------------------------------------------------------------
 */
bool
ExecQual(List *qual, ExprContext *econtext, bool resultForNull)
{
	bool		result;
	MemoryContext oldContext;
	List	   *qlist;

	/*
	 * debugging stuff
	 */
	EV_printf("ExecQual: qual is ");
	EV_nodeDisplay(qual);
	EV_printf("\n");

	IncrProcessed();

	/*
	 * Run in short-lived per-tuple context while computing expressions.
	 */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * Evaluate the qual conditions one at a time.	If we find a FALSE
	 * result, we can stop evaluating and return FALSE --- the AND result
	 * must be FALSE.  Also, if we find a NULL result when resultForNull
	 * is FALSE, we can stop and return FALSE --- the AND result must be
	 * FALSE or NULL in that case, and the caller doesn't care which.
	 *
	 * If we get to the end of the list, we can return TRUE.  This will
	 * happen when the AND result is indeed TRUE, or when the AND result
	 * is NULL (one or more NULL subresult, with all the rest TRUE) and
	 * the caller has specified resultForNull = TRUE.
	 */
	result = true;

	foreach(qlist, qual)
	{
		Node	   *clause = (Node *) lfirst(qlist);
		Datum		expr_value;
		bool		isNull;

		expr_value = ExecEvalExpr(clause, econtext, &isNull, NULL);

		if (isNull)
		{
			if (resultForNull == false)
			{
				result = false; /* treat NULL as FALSE */
				break;
			}
		}
		else
		{
			if (!DatumGetBool(expr_value))
			{
				result = false; /* definitely FALSE */
				break;
			}
		}
	}

	MemoryContextSwitchTo(oldContext);

	return result;
}

/*
 * Number of items in a tlist (including any resjunk items!)
 */
int
ExecTargetListLength(List *targetlist)
{
	int			len = 0;
	List	   *tl;

	foreach(tl, targetlist)
	{
		TargetEntry *curTle = (TargetEntry *) lfirst(tl);

		if (curTle->resdom != NULL)
			len++;
		else
			len += curTle->fjoin->fj_nNodes;
	}
	return len;
}

/*
 * Number of items in a tlist, not including any resjunk items
 */
int
ExecCleanTargetListLength(List *targetlist)
{
	int			len = 0;
	List	   *tl;

	foreach(tl, targetlist)
	{
		TargetEntry *curTle = (TargetEntry *) lfirst(tl);

		if (curTle->resdom != NULL)
		{
			if (!curTle->resdom->resjunk)
				len++;
		}
		else
			len += curTle->fjoin->fj_nNodes;
	}
	return len;
}

/* ----------------------------------------------------------------
 *		ExecTargetList
 *
 *		Evaluates a targetlist with respect to the current
 *		expression context and return a tuple.
 *
 * As with ExecEvalExpr, the caller should pass isDone = NULL if not
 * prepared to deal with sets of result tuples.  Otherwise, a return
 * of *isDone = ExprMultipleResult signifies a set element, and a return
 * of *isDone = ExprEndResult signifies end of the set of tuple.
 * ----------------------------------------------------------------
 */
static HeapTuple
ExecTargetList(List *targetlist,
			   int nodomains,
			   TupleDesc targettype,
			   Datum *values,
			   ExprContext *econtext,
			   ExprDoneCond *isDone)
{
	MemoryContext oldContext;

#define NPREALLOCDOMAINS 64
	char		nullsArray[NPREALLOCDOMAINS];
	bool		fjIsNullArray[NPREALLOCDOMAINS];
	ExprDoneCond itemIsDoneArray[NPREALLOCDOMAINS];
	char	   *nulls;
	bool	   *fjIsNull;
	ExprDoneCond *itemIsDone;
	List	   *tl;
	TargetEntry *tle;
	AttrNumber	resind;
	HeapTuple	newTuple;
	bool		isNull;
	bool		haveDoneSets;
	static struct tupleDesc NullTupleDesc;		/* we assume this inits to
												 * zeroes */

	/*
	 * debugging stuff
	 */
	EV_printf("ExecTargetList: tl is ");
	EV_nodeDisplay(targetlist);
	EV_printf("\n");

	/*
	 * Run in short-lived per-tuple context while computing expressions.
	 */
	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	/*
	 * There used to be some klugy and demonstrably broken code here that
	 * special-cased the situation where targetlist == NIL.  Now we just
	 * fall through and return an empty-but-valid tuple.  We do, however,
	 * have to cope with the possibility that targettype is NULL ---
	 * heap_formtuple won't like that, so pass a dummy descriptor with
	 * natts = 0 to deal with it.
	 */
	if (targettype == NULL)
		targettype = &NullTupleDesc;

	/*
	 * allocate an array of char's to hold the "null" information only if
	 * we have a really large targetlist.  otherwise we use the stack.
	 *
	 * We also allocate a bool array that is used to hold fjoin result state,
	 * and another array that holds the isDone status for each targetlist
	 * item. The isDone status is needed so that we can iterate,
	 * generating multiple tuples, when one or more tlist items return
	 * sets.  (We expect the caller to call us again if we return:
	 *
	 * isDone = ExprMultipleResult.)
	 */
	if (nodomains > NPREALLOCDOMAINS)
	{
		nulls = (char *) palloc(nodomains * sizeof(char));
		fjIsNull = (bool *) palloc(nodomains * sizeof(bool));
		itemIsDone = (ExprDoneCond *) palloc(nodomains * sizeof(ExprDoneCond));
	}
	else
	{
		nulls = nullsArray;
		fjIsNull = fjIsNullArray;
		itemIsDone = itemIsDoneArray;
	}

	/*
	 * evaluate all the expressions in the target list
	 */

	if (isDone)
		*isDone = ExprSingleResult;		/* until proven otherwise */

	haveDoneSets = false;		/* any exhausted set exprs in tlist? */

	foreach(tl, targetlist)
	{
		tle = lfirst(tl);

		if (tle->resdom != NULL)
		{
			resind = tle->resdom->resno - 1;

			values[resind] = ExecEvalExpr(tle->expr,
										  econtext,
										  &isNull,
										  &itemIsDone[resind]);
			nulls[resind] = isNull ? 'n' : ' ';

			if (itemIsDone[resind] != ExprSingleResult)
			{
				/* We have a set-valued expression in the tlist */
				if (isDone == NULL)
					elog(ERROR, "Set-valued function called in context that cannot accept a set");
				if (itemIsDone[resind] == ExprMultipleResult)
				{
					/* we have undone sets in the tlist, set flag */
					*isDone = ExprMultipleResult;
				}
				else
				{
					/* we have done sets in the tlist, set flag for that */
					haveDoneSets = true;
				}
			}
		}
		else
		{
#ifdef SETS_FIXED
			int			curNode;
			Resdom	   *fjRes;
			List	   *fjTlist = (List *) tle->expr;
			Fjoin	   *fjNode = tle->fjoin;
			int			nNodes = fjNode->fj_nNodes;
			DatumPtr	results = fjNode->fj_results;

			ExecEvalFjoin(tle, econtext, fjIsNull, isDone);

			/*
			 * XXX this is wrong, but since fjoin code is completely
			 * broken anyway, I'm not going to worry about it now --- tgl
			 * 8/23/00
			 */
			if (isDone && *isDone == ExprEndResult)
			{
				MemoryContextSwitchTo(oldContext);
				newTuple = NULL;
				goto exit;
			}

			/*
			 * get the result from the inner node
			 */
			fjRes = (Resdom *) fjNode->fj_innerNode;
			resind = fjRes->resno - 1;
			values[resind] = results[0];
			nulls[resind] = fjIsNull[0] ? 'n' : ' ';

			/*
			 * Get results from all of the outer nodes
			 */
			for (curNode = 1;
				 curNode < nNodes;
				 curNode++, fjTlist = lnext(fjTlist))
			{
				Node	   *outernode = lfirst(fjTlist);

				fjRes = (Resdom *) outernode->iterexpr;
				resind = fjRes->resno - 1;
				values[resind] = results[curNode];
				nulls[resind] = fjIsNull[curNode] ? 'n' : ' ';
			}
#else
			elog(ERROR, "ExecTargetList: fjoin nodes not currently supported");
#endif
		}
	}

	if (haveDoneSets)
	{
		/*
		 * note: can't get here unless we verified isDone != NULL
		 */
		if (*isDone == ExprSingleResult)
		{
			/*
			 * all sets are done, so report that tlist expansion is
			 * complete.
			 */
			*isDone = ExprEndResult;
			MemoryContextSwitchTo(oldContext);
			newTuple = NULL;
			goto exit;
		}
		else
		{
			/*
			 * We have some done and some undone sets.	Restart the done
			 * ones so that we can deliver a tuple (if possible).
			 */
			foreach(tl, targetlist)
			{
				tle = lfirst(tl);

				if (tle->resdom != NULL)
				{
					resind = tle->resdom->resno - 1;

					if (itemIsDone[resind] == ExprEndResult)
					{
						values[resind] = ExecEvalExpr(tle->expr,
													  econtext,
													  &isNull,
													&itemIsDone[resind]);
						nulls[resind] = isNull ? 'n' : ' ';

						if (itemIsDone[resind] == ExprEndResult)
						{
							/*
							 * Oh dear, this item is returning an empty
							 * set. Guess we can't make a tuple after all.
							 */
							*isDone = ExprEndResult;
							break;
						}
					}
				}
			}

			/*
			 * If we cannot make a tuple because some sets are empty, we
			 * still have to cycle the nonempty sets to completion, else
			 * resources will not be released from subplans etc.
			 */
			if (*isDone == ExprEndResult)
			{
				foreach(tl, targetlist)
				{
					tle = lfirst(tl);

					if (tle->resdom != NULL)
					{
						resind = tle->resdom->resno - 1;

						while (itemIsDone[resind] == ExprMultipleResult)
						{
							(void) ExecEvalExpr(tle->expr,
												econtext,
												&isNull,
												&itemIsDone[resind]);
						}
					}
				}

				MemoryContextSwitchTo(oldContext);
				newTuple = NULL;
				goto exit;
			}
		}
	}

	/*
	 * form the new result tuple (in the caller's memory context!)
	 */
	MemoryContextSwitchTo(oldContext);

	newTuple = (HeapTuple) heap_formtuple(targettype, values, nulls);

exit:

	/*
	 * free the status arrays if we palloc'd them
	 */
	if (nodomains > NPREALLOCDOMAINS)
	{
		pfree(nulls);
		pfree(fjIsNull);
		pfree(itemIsDone);
	}

	return newTuple;
}

/* ----------------------------------------------------------------
 *		ExecProject
 *
 *		projects a tuple based on projection info and stores
 *		it in the specified tuple table slot.
 *
 *		Note: someday soon the executor can be extended to eliminate
 *			  redundant projections by storing pointers to datums
 *			  in the tuple table and then passing these around when
 *			  possible.  this should make things much quicker.
 *			  -cim 6/3/91
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecProject(ProjectionInfo *projInfo, ExprDoneCond *isDone)
{
	TupleTableSlot *slot;
	List	   *targetlist;
	int			len;
	TupleDesc	tupType;
	Datum	   *tupValue;
	ExprContext *econtext;
	HeapTuple	newTuple;

	/*
	 * sanity checks
	 */
	if (projInfo == NULL)
		return (TupleTableSlot *) NULL;

	/*
	 * get the projection info we want
	 */
	slot = projInfo->pi_slot;
	targetlist = projInfo->pi_targetlist;
	len = projInfo->pi_len;
	tupType = slot->ttc_tupleDescriptor;

	tupValue = projInfo->pi_tupValue;
	econtext = projInfo->pi_exprContext;

	/*
	 * form a new result tuple (if possible --- result can be NULL)
	 */
	newTuple = ExecTargetList(targetlist,
							  len,
							  tupType,
							  tupValue,
							  econtext,
							  isDone);

	/*
	 * store the tuple in the projection slot and return the slot.
	 */
	return ExecStoreTuple(newTuple,		/* tuple to store */
						  slot, /* slot to store in */
						  InvalidBuffer,		/* tuple has no buffer */
						  true);
}
