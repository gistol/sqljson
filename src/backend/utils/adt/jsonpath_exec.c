/*-------------------------------------------------------------------------
 *
 * jsonpath_exec.c
 *	 Routines for SQL/JSON path execution.
 *
 * Jsonpath is executed in the global context stored in JsonPathExecContext,
 * which is passed to almost every function involved into execution.  Entry
 * point for jsonpath execution is executeJsonPath() function, which
 * initializes execution context including initial JsonPathItem and JsonbValue,
 * flags, stack for calculation of @ in filters.
 *
 * The result of jsonpath query execution is enum JsonPathExecResult and
 * if succeeded sequence of JsonbValue, written to JsonValueList *found, which
 * is passed through the jsonpath items.  When found == NULL, we're inside
 * exists-query and we're interested only in whether result is empty.  In this
 * case execution is stopped once first result item is found, and the only
 * execution result is JsonPathExecResult.  The values of JsonPathExecResult
 * are following:
 * - jperOk			-- result sequence is not empty
 * - jperNotFound	-- result sequence is empty
 * - jperError		-- error occurred during execution
 *
 * Jsonpath is executed recursively (see executeItem()) starting form the
 * first path item (which in turn might be, for instance, an arithmetic
 * expression evaluated separately).  On each step single JsonbValue obtained
 * from previous path item is processed.  The result of processing is a
 * sequence of JsonbValue (probably empty), which is passed to the next path
 * item one by one.  When there is no next path item, then JsonbValue is added
 * to the 'found' list.  When found == NULL, then execution functions just
 * return jperOk (see executeNextItem()).
 *
 * Many of jsonpath operations require automatic unwrapping of arrays in lax
 * mode.  So, if input value is array, then corresponding operation is
 * processed not on array itself, but on all of its members one by one.
 * executeItemOptUnwrapTarget() function have 'unwrap' argument, which indicates
 * whether unwrapping of array is needed.  When unwrap == true, each of array
 * members is passed to executeItemOptUnwrapTarget() again but with unwrap == false
 * in order to evade subsequent array unwrapping.
 *
 * All boolean expressions (predicates) are evaluated by executeBoolItem()
 * function, which returns tri-state JsonPathBool.  When error is occurred
 * during predicate execution, it returns jpbUnknown.  According to standard
 * predicates can be only inside filters.  But we support their usage as
 * jsonpath expression.  This helps us to implement @@ operator.  In this case
 * resulting JsonPathBool is transformed into jsonb bool or null.
 *
 * Arithmetic and boolean expression are evaluated recursively from expression
 * tree top down to the leaves.  Therefore, for binary arithmetic expressions
 * we calculate operands first.  Then we check that results are numeric
 * singleton lists, calculate the result and pass it to the next path item.
 *
 * Copyright (c) 2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/jsonpath_exec.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "regex/regex.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/datum.h"
#include "utils/formatting.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/jsonapi.h"
#include "utils/jsonpath.h"
#include "utils/date.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"


/* Standard error message for SQL/JSON errors */
#define ERRMSG_JSON_ARRAY_NOT_FOUND			"SQL/JSON array not found"
#define ERRMSG_JSON_OBJECT_NOT_FOUND		"SQL/JSON object not found"
#define ERRMSG_JSON_MEMBER_NOT_FOUND		"SQL/JSON member not found"
#define ERRMSG_JSON_NUMBER_NOT_FOUND		"SQL/JSON number not found"
#define ERRMSG_JSON_SCALAR_REQUIRED			"SQL/JSON scalar required"
#define ERRMSG_SINGLETON_JSON_ITEM_REQUIRED	"singleton SQL/JSON item required"
#define ERRMSG_NON_NUMERIC_JSON_ITEM		"non-numeric SQL/JSON item"
#define ERRMSG_INVALID_JSON_SUBSCRIPT		"invalid SQL/JSON subscript"
#define ERRMSG_INVALID_ARGUMENT_FOR_JSON_DATETIME_FUNCTION	\
	"invalid argument for SQL/JSON datetime function"

typedef enum JsonItemType
{
	/* Scalar types */
	jsiNull = jbvNull,
	jsiString = jbvString,
	jsiNumeric = jbvNumeric,
	jsiBool = jbvBool,
	/* Composite types */
	jsiArray = jbvArray,
	jsiObject = jbvObject,
	/* Binary (i.e. struct Jsonb) jbvArray/jbvObject */
	jsiBinary = jbvBinary,

	/*
	 * Virtual types.
	 *
	 * These types are used only for in-memory JSON processing and serialized
	 * into JSON strings when outputted to json/jsonb.
	 */
	jsiDatetime = 0x20
} JsonItemType;

/* SQL/JSON item */
typedef union JsonItem
{
	int			type;	/* XXX JsonItemType */

	JsonbValue	jbv;

	struct
	{
		enum jbvType type;
		Datum		value;
		Oid			typid;
		int32		typmod;
		int			tz;
	}			datetime;
} JsonItem;

#define JsonbValueToJsonItem(jbv) ((JsonItem *) (jbv))

/*
 * Represents "base object" and it's "id" for .keyvalue() evaluation.
 */
typedef struct JsonBaseObjectInfo
{
	JsonbContainer *jbc;
	int			id;
} JsonBaseObjectInfo;

/*
 * Special data structure representing stack of current items.  We use it
 * instead of regular list in order to evade extra memory allocation.  These
 * items are always allocated in local variables.
 */
typedef struct JsonItemStackEntry
{
	JsonItem   *item;
	struct JsonItemStackEntry *parent;
} JsonItemStackEntry;

typedef JsonItemStackEntry *JsonItemStack;

typedef int (*JsonPathVarCallback) (void *vars, char *varName, int varNameLen,
									JsonItem *val, JsonbValue *baseObject);

/*
 * Context of jsonpath execution.
 */
typedef struct JsonPathExecContext
{
	void	   *vars;			/* variables to substitute into jsonpath */
	JsonPathVarCallback getVar;
	JsonItem   *root;			/* for $ evaluation */
	JsonItemStack stack;		/* for @ evaluation */
	JsonBaseObjectInfo baseObject;	/* "base object" for .keyvalue()
									 * evaluation */
	int			lastGeneratedObjectId;	/* "id" counter for .keyvalue()
										 * evaluation */
	int			innermostArraySize; /* for LAST array index evaluation */
	bool		laxMode;		/* true for "lax" mode, false for "strict"
								 * mode */
	bool		ignoreStructuralErrors; /* with "true" structural errors such
										 * as absence of required json item or
										 * unexpected json item type are
										 * ignored */
	bool		throwErrors;	/* with "false" all suppressible errors are
								 * suppressed */
} JsonPathExecContext;

/* Context for LIKE_REGEX execution. */
typedef struct JsonLikeRegexContext
{
	text	   *regex;
	int			cflags;
} JsonLikeRegexContext;

/* Result of jsonpath predicate evaluation */
typedef enum JsonPathBool
{
	jpbFalse = 0,
	jpbTrue = 1,
	jpbUnknown = 2
} JsonPathBool;

/* Result of jsonpath expression evaluation */
typedef enum JsonPathExecResult
{
	jperOk = 0,
	jperNotFound = 1,
	jperError = 2
} JsonPathExecResult;

#define jperIsError(jper)			((jper) == jperError)

/*
 * List of SQL/JSON items with shortcut for single-value list.
 */
typedef struct JsonValueList
{
	JsonItem   *singleton;
	List	   *list;
} JsonValueList;

typedef struct JsonValueListIterator
{
	JsonItem   *value;
	ListCell   *next;
} JsonValueListIterator;

/*
 * Context for execution of
 * jsonb_path_*(jsonb, jsonpath [, vars jsonb, silent boolean]) user functions.
 */
typedef struct JsonPathUserFuncContext
{
	FunctionCallInfo fcinfo;
	Jsonb	   *jb;				/* first jsonb function argument */
	JsonPath   *jp;				/* second jsonpath function argument */
	Jsonb	   *vars;			/* third vars function argument */
	JsonValueList found;		/* resulting item list */
	bool		silent;			/* error suppression flag */
} JsonPathUserFuncContext;

/* strict/lax flags is decomposed into four [un]wrap/error flags */
#define jspStrictAbsenseOfErrors(cxt)	(!(cxt)->laxMode)
#define jspAutoUnwrap(cxt)				((cxt)->laxMode)
#define jspAutoWrap(cxt)				((cxt)->laxMode)
#define jspIgnoreStructuralErrors(cxt)	((cxt)->ignoreStructuralErrors)
#define jspThrowErrors(cxt)				((cxt)->throwErrors)

/* Convenience macro: return or throw error depending on context */
#define RETURN_ERROR(throw_error) \
do { \
	if (jspThrowErrors(cxt)) \
		throw_error; \
	else \
		return jperError; \
} while (0)

typedef JsonPathBool (*JsonPathPredicateCallback) (JsonPathItem *jsp,
												   JsonItem *larg,
												   JsonItem *rarg,
												   void *param);
typedef Numeric (*BinaryArithmFunc) (Numeric num1, Numeric num2, bool *error);

static void freeUserFuncContext(JsonPathUserFuncContext *cxt);
static JsonPathExecResult executeUserFunc(FunctionCallInfo fcinfo,
				JsonPathUserFuncContext *cxt, bool copy);

static JsonPathExecResult executeJsonPath(JsonPath *path, void *vars,
				JsonPathVarCallback getVar, Jsonb *json, bool throwErrors,
				JsonValueList *result);
static JsonPathExecResult executeItem(JsonPathExecContext *cxt,
			JsonPathItem *jsp, JsonItem *jb, JsonValueList *found);
static JsonPathExecResult executeItemOptUnwrapTarget(JsonPathExecContext *cxt,
						   JsonPathItem *jsp, JsonItem *jb,
						   JsonValueList *found, bool unwrap);
static JsonPathExecResult executeItemUnwrapTargetArray(JsonPathExecContext *cxt,
							 JsonPathItem *jsp, JsonItem *jb,
							 JsonValueList *found, bool unwrapElements);
static JsonPathExecResult executeNextItem(JsonPathExecContext *cxt,
				JsonPathItem *cur, JsonPathItem *next,
				JsonItem *v, JsonValueList *found, bool copy);
static JsonPathExecResult executeItemOptUnwrapResult(
						   JsonPathExecContext *cxt, JsonPathItem *jsp,
						   JsonItem *jb, bool unwrap, JsonValueList *found);
static JsonPathExecResult executeItemOptUnwrapResultNoThrow(
								  JsonPathExecContext *cxt, JsonPathItem *jsp,
								  JsonItem *jb, bool unwrap,
								  JsonValueList *found);
static JsonPathBool executeBoolItem(JsonPathExecContext *cxt,
				JsonPathItem *jsp, JsonItem *jb, bool canHaveNext);
static JsonPathBool executeNestedBoolItem(JsonPathExecContext *cxt,
					  JsonPathItem *jsp, JsonItem *jb);
static JsonPathExecResult executeAnyItem(JsonPathExecContext *cxt,
			   JsonPathItem *jsp, JsonbContainer *jbc, JsonValueList *found,
			   uint32 level, uint32 first, uint32 last,
			   bool ignoreStructuralErrors, bool unwrapNext);
static JsonPathBool executePredicate(JsonPathExecContext *cxt,
				 JsonPathItem *pred, JsonPathItem *larg, JsonPathItem *rarg,
				 JsonItem *jb, bool unwrapRightArg,
				 JsonPathPredicateCallback exec, void *param);
static JsonPathExecResult executeBinaryArithmExpr(JsonPathExecContext *cxt,
						JsonPathItem *jsp, JsonItem *jb, BinaryArithmFunc func,
						JsonValueList *found);
static JsonPathExecResult executeUnaryArithmExpr(JsonPathExecContext *cxt,
					   JsonPathItem *jsp, JsonItem *jb, PGFunction func,
					   JsonValueList *found);
static JsonPathBool executeStartsWith(JsonPathItem *jsp,
				  JsonItem *whole, JsonItem *initial, void *param);
static JsonPathBool executeLikeRegex(JsonPathItem *jsp, JsonItem *str,
				 JsonItem *rarg, void *param);
static JsonPathExecResult executeNumericItemMethod(JsonPathExecContext *cxt,
						 JsonPathItem *jsp, JsonItem *jb, bool unwrap, PGFunction func,
						 JsonValueList *found);
static JsonPathExecResult executeKeyValueMethod(JsonPathExecContext *cxt,
					  JsonPathItem *jsp, JsonItem *jb, JsonValueList *found);
static JsonPathExecResult appendBoolResult(JsonPathExecContext *cxt,
				 JsonPathItem *jsp, JsonValueList *found, JsonPathBool res);
static void getJsonPathItem(JsonPathExecContext *cxt, JsonPathItem *item,
				JsonItem *value);
static void getJsonPathVariable(JsonPathExecContext *cxt,
					JsonPathItem *variable, JsonItem *value);
static int getJsonPathVariableFromJsonb(void *varsJsonb, char *varName,
							 int varNameLen, JsonItem *val,
							 JsonbValue *baseObject);
static int	JsonbArraySize(JsonItem *jb);
static JsonPathBool executeComparison(JsonPathItem *cmp, JsonItem *lv,
				  JsonItem *rv, void *p);
static JsonPathBool compareItems(int32 op, JsonItem *jb1, JsonItem *jb2);
static int	compareNumeric(Numeric a, Numeric b);
static JsonItem *copyJsonItem(JsonItem *src);
static JsonbValue *JsonItemToJsonbValue(JsonItem *jsi, JsonbValue *jbv);
static Jsonb *JsonItemToJsonb(JsonItem *jsi);
static const char *JsonItemTypeName(JsonItem *jsi);
static JsonPathExecResult getArrayIndex(JsonPathExecContext *cxt,
			  JsonPathItem *jsp, JsonItem *jb, int32 *index);
static JsonBaseObjectInfo setBaseObject(JsonPathExecContext *cxt,
			  JsonItem *jsi, int32 id);
static void JsonValueListAppend(JsonValueList *jvl, JsonItem *jbv);
static int	JsonValueListLength(const JsonValueList *jvl);
static bool JsonValueListIsEmpty(JsonValueList *jvl);
static JsonItem *JsonValueListHead(JsonValueList *jvl);
static List *JsonValueListGetList(JsonValueList *jvl);
static void JsonValueListInitIterator(const JsonValueList *jvl,
						  JsonValueListIterator *it);
static JsonItem *JsonValueListNext(const JsonValueList *jvl,
				  JsonValueListIterator *it);
static int	JsonbType(JsonItem *jb);
static JsonbValue *JsonbInitBinary(JsonbValue *jbv, Jsonb *jb);
static JsonItem *getScalar(JsonItem *scalar, enum jbvType type);
static JsonbValue *wrapItemsInArray(const JsonValueList *items);
static text *JsonItemUnquoteText(JsonItem *jsi);

static bool tryToParseDatetime(text *fmt, text *datetime, char *tzname,
				   bool strict, Datum *value, Oid *typid,
				   int32 *typmod, int *tzp, bool throwErrors);
static int compareDatetime(Datum val1, Oid typid1, int tz1,
				Datum val2, Oid typid2, int tz2,
				bool *error);

static void pushJsonItem(JsonItemStack *stack,
			 JsonItemStackEntry *entry, JsonItem *item);
static void popJsonItem(JsonItemStack *stack);

/****************** User interface to JsonPath executor ********************/

/*
 * jsonb_path_exists
 *		Returns true if jsonpath returns at least one item for the specified
 *		jsonb value.  This function and jsonb_path_match() are used to
 *		implement @? and @@ operators, which in turn are intended to have an
 *		index support.  Thus, it's desirable to make it easier to achieve
 *		consistency between index scan results and sequential scan results.
 *		So, we throw as less errors as possible.  Regarding this function,
 *		such behavior also matches behavior of JSON_EXISTS() clause of
 *		SQL/JSON.  Regarding jsonb_path_match(), this function doesn't have
 *		an analogy in SQL/JSON, so we define its behavior on our own.
 */
Datum
jsonb_path_exists(PG_FUNCTION_ARGS)
{
	JsonPathExecResult res = executeUserFunc(fcinfo, NULL, false);

	if (jperIsError(res))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(res == jperOk);
}

/*
 * jsonb_path_exists_opr
 *		Implementation of operator "jsonb @? jsonpath" (2-argument version of
 *		jsonb_path_exists()).
 */
Datum
jsonb_path_exists_opr(PG_FUNCTION_ARGS)
{
	/* just call the other one -- it can handle both cases */
	return jsonb_path_exists(fcinfo);
}

/*
 * jsonb_path_match
 *		Returns jsonpath predicate result item for the specified jsonb value.
 *		See jsonb_path_exists() comment for details regarding error handling.
 */
Datum
jsonb_path_match(PG_FUNCTION_ARGS)
{
	JsonPathUserFuncContext cxt;

	(void) executeUserFunc(fcinfo, &cxt, false);

	freeUserFuncContext(&cxt);

	if (JsonValueListLength(&cxt.found) == 1)
	{
		JsonItem   *jsi = JsonValueListHead(&cxt.found);

		if (jsi->type == jbvBool)
			PG_RETURN_BOOL(jsi->jbv.val.boolean);

		if (jsi->type == jbvNull)
			PG_RETURN_NULL();
	}

	if (!cxt.silent)
		ereport(ERROR,
				(errcode(ERRCODE_SINGLETON_JSON_ITEM_REQUIRED),
				 errmsg(ERRMSG_SINGLETON_JSON_ITEM_REQUIRED),
				 errdetail("expression should return a singleton boolean")));

	PG_RETURN_NULL();
}

/*
 * jsonb_path_match_opr
 *		Implementation of operator "jsonb @@ jsonpath" (2-argument version of
 *		jsonb_path_match()).
 */
Datum
jsonb_path_match_opr(PG_FUNCTION_ARGS)
{
	/* just call the other one -- it can handle both cases */
	return jsonb_path_match(fcinfo);
}

/*
 * jsonb_path_query
 *		Executes jsonpath for given jsonb document and returns result as
 *		rowset.
 */
Datum
jsonb_path_query(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *found;
	JsonItem   *v;
	ListCell   *c;

	if (SRF_IS_FIRSTCALL())
	{
		JsonPathUserFuncContext jspcxt;
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* jsonb and jsonpath arguments are copied into SRF context. */
		(void) executeUserFunc(fcinfo, &jspcxt, true);

		/*
		 * Don't free jspcxt because items in jspcxt.found can reference
		 * untoasted copies of jsonb and jsonpath arguments.
		 */

		funcctx->user_fctx = JsonValueListGetList(&jspcxt.found);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	found = funcctx->user_fctx;

	c = list_head(found);

	if (c == NULL)
		SRF_RETURN_DONE(funcctx);

	v = lfirst(c);
	funcctx->user_fctx = list_delete_first(found);

	SRF_RETURN_NEXT(funcctx, JsonbPGetDatum(JsonItemToJsonb(v)));
}

/*
 * jsonb_path_query_array
 *		Executes jsonpath for given jsonb document and returns result as
 *		jsonb array.
 */
Datum
jsonb_path_query_array(FunctionCallInfo fcinfo)
{
	JsonPathUserFuncContext cxt;
	Jsonb	   *jb;

	(void) executeUserFunc(fcinfo, &cxt, false);

	jb = JsonbValueToJsonb(wrapItemsInArray(&cxt.found));

	freeUserFuncContext(&cxt);

	PG_RETURN_JSONB_P(jb);
}

/*
 * jsonb_path_query_first
 *		Executes jsonpath for given jsonb document and returns first result
 *		item.  If there are no items, NULL returned.
 */
Datum
jsonb_path_query_first(FunctionCallInfo fcinfo)
{
	JsonPathUserFuncContext cxt;
	Jsonb	   *jb;

	(void) executeUserFunc(fcinfo, &cxt, false);

	if (JsonValueListLength(&cxt.found) >= 1)
		jb = JsonItemToJsonb(JsonValueListHead(&cxt.found));
	else
		jb = NULL;

	freeUserFuncContext(&cxt);

	if (jb)
		PG_RETURN_JSONB_P(jb);
	else
		PG_RETURN_NULL();
}

/*
 * jsonb_path_query_first_text
 *		Executes jsonpath for given jsonb document and returns first result
 *		item as text.  If there are no items, NULL returned.
 */
Datum
jsonb_path_query_first_text(FunctionCallInfo fcinfo)
{
	JsonPathUserFuncContext cxt;
	text	   *txt;

	(void) executeUserFunc(fcinfo, &cxt, false);

	if (JsonValueListLength(&cxt.found) >= 1)
		txt = JsonItemUnquoteText(JsonValueListHead(&cxt.found));
	else
		txt = NULL;

	freeUserFuncContext(&cxt);

	if (txt)
		PG_RETURN_TEXT_P(txt);
	else
		PG_RETURN_NULL();
}

/* Free untoasted copies of jsonb and jsonpath arguments. */
static void
freeUserFuncContext(JsonPathUserFuncContext *cxt)
{
	FunctionCallInfo fcinfo = cxt->fcinfo;

	PG_FREE_IF_COPY(cxt->jb, 0);
	PG_FREE_IF_COPY(cxt->jp, 1);
	if (cxt->vars)
		PG_FREE_IF_COPY(cxt->vars, 2);
}

/*
 * Common code for jsonb_path_*(jsonb, jsonpath [, vars jsonb, silent bool])
 * user functions.
 *
 * 'copy' flag enables copying of first three arguments into the current memory
 * context.
 */
static JsonPathExecResult
executeUserFunc(FunctionCallInfo fcinfo, JsonPathUserFuncContext *cxt,
				bool copy)
{
	Jsonb	   *jb = copy ? PG_GETARG_JSONB_P_COPY(0) : PG_GETARG_JSONB_P(0);
	JsonPath   *jp = copy ? PG_GETARG_JSONPATH_P_COPY(1) : PG_GETARG_JSONPATH_P(1);
	Jsonb	   *vars = NULL;
	bool		silent = true;
	JsonPathExecResult res;

	if (PG_NARGS() == 4)
	{
		vars = copy ? PG_GETARG_JSONB_P_COPY(2) : PG_GETARG_JSONB_P(2);
		silent = PG_GETARG_BOOL(3);
	}

	if (cxt)
	{
		cxt->fcinfo = fcinfo;
		cxt->jb = jb;
		cxt->jp = jp;
		cxt->vars = vars;
		cxt->silent = silent;
		memset(&cxt->found, 0, sizeof(cxt->found));
	}

	res = executeJsonPath(jp, vars, getJsonPathVariableFromJsonb, jb,
						  !silent, cxt ? &cxt->found : NULL);

	if (!cxt && !copy)
	{
		PG_FREE_IF_COPY(jb, 0);
		PG_FREE_IF_COPY(jp, 1);
		if (vars)
			PG_FREE_IF_COPY(vars, 2);
	}

	return res;
}

/********************Execute functions for JsonPath**************************/

/*
 * Interface to jsonpath executor
 *
 * 'path' - jsonpath to be executed
 * 'vars' - variables to be substituted to jsonpath
 * 'json' - target document for jsonpath evaluation
 * 'throwErrors' - whether we should throw suppressible errors
 * 'result' - list to store result items into
 *
 * Returns an error happens during processing or NULL on no error.
 *
 * Note, jsonb and jsonpath values should be avaliable and untoasted during
 * work because JsonPathItem, JsonbValue and result item could have pointers
 * into input values.  If caller needs to just check if document matches
 * jsonpath, then it doesn't provide a result arg.  In this case executor
 * works till first positive result and does not check the rest if possible.
 * In other case it tries to find all the satisfied result items.
 */
static JsonPathExecResult
executeJsonPath(JsonPath *path, void *vars, JsonPathVarCallback getVar,
				Jsonb *json, bool throwErrors, JsonValueList *result)
{
	JsonPathExecContext cxt;
	JsonPathExecResult res;
	JsonPathItem jsp;
	JsonItem	jsi;
	JsonItemStackEntry root;

	jspInit(&jsp, path);

	if (!JsonbExtractScalar(&json->root, &jsi.jbv))
		JsonbInitBinary(&jsi.jbv, json);

	cxt.vars = vars;
	cxt.getVar = getVar;
	cxt.laxMode = (path->header & JSONPATH_LAX) != 0;
	cxt.ignoreStructuralErrors = cxt.laxMode;
	cxt.root = &jsi;
	cxt.stack = NULL;
	cxt.baseObject.jbc = NULL;
	cxt.baseObject.id = 0;
	/* 1 + number of base objects in vars */
	cxt.lastGeneratedObjectId = 1 + getVar(vars, NULL, 0, NULL, NULL);
	cxt.innermostArraySize = -1;
	cxt.throwErrors = throwErrors;

	pushJsonItem(&cxt.stack, &root, cxt.root);

	if (jspStrictAbsenseOfErrors(&cxt) && !result)
	{
		/*
		 * In strict mode we must get a complete list of values to check that
		 * there are no errors at all.
		 */
		JsonValueList vals = {0};

		res = executeItem(&cxt, &jsp, &jsi, &vals);

		if (jperIsError(res))
			return res;

		return JsonValueListIsEmpty(&vals) ? jperNotFound : jperOk;
	}

	res = executeItem(&cxt, &jsp, &jsi, result);

	Assert(!throwErrors || !jperIsError(res));

	return res;
}

/*
 * Execute jsonpath with automatic unwrapping of current item in lax mode.
 */
static JsonPathExecResult
executeItem(JsonPathExecContext *cxt, JsonPathItem *jsp,
			JsonItem *jb, JsonValueList *found)
{
	return executeItemOptUnwrapTarget(cxt, jsp, jb, found, jspAutoUnwrap(cxt));
}

/*
 * Main jsonpath executor function: walks on jsonpath structure, finds
 * relevant parts of jsonb and evaluates expressions over them.
 * When 'unwrap' is true current SQL/JSON item is unwrapped if it is an array.
 */
static JsonPathExecResult
executeItemOptUnwrapTarget(JsonPathExecContext *cxt, JsonPathItem *jsp,
						   JsonItem *jb, JsonValueList *found, bool unwrap)
{
	JsonPathItem elem;
	JsonPathExecResult res = jperNotFound;
	JsonBaseObjectInfo baseObject;

	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	switch (jsp->type)
	{
			/* all boolean item types: */
		case jpiAnd:
		case jpiOr:
		case jpiNot:
		case jpiIsUnknown:
		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
		case jpiExists:
		case jpiStartsWith:
		case jpiLikeRegex:
			{
				JsonPathBool st = executeBoolItem(cxt, jsp, jb, true);

				res = appendBoolResult(cxt, jsp, found, st);
				break;
			}

		case jpiKey:
			if (JsonbType(jb) == jbvObject)
			{
				JsonbValue *v;
				JsonbValue	key;

				key.type = jbvString;
				key.val.string.val = jspGetString(jsp, &key.val.string.len);

				v = findJsonbValueFromContainer(jb->jbv.val.binary.data,
												JB_FOBJECT, &key);

				if (v != NULL)
				{
					jb = JsonbValueToJsonItem(v);
					res = executeNextItem(cxt, jsp, NULL, jb, found, false);

					/* free value if it was not added to found list */
					if (jspHasNext(jsp) || !found)
						pfree(v);
				}
				else if (!jspIgnoreStructuralErrors(cxt))
				{
					StringInfoData keybuf;
					char	   *keystr;

					Assert(found);

					if (!jspThrowErrors(cxt))
						return jperError;

					initStringInfo(&keybuf);

					keystr = pnstrdup(key.val.string.val, key.val.string.len);
					escape_json(&keybuf, keystr);

					ereport(ERROR,
							(errcode(ERRCODE_JSON_MEMBER_NOT_FOUND),
							 errmsg(ERRMSG_JSON_MEMBER_NOT_FOUND),
							 errdetail("JSON object does not contain key %s",
									   keybuf.data)));
				}
			}
			else if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);
			else if (!jspIgnoreStructuralErrors(cxt))
			{
				Assert(found);
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_JSON_MEMBER_NOT_FOUND),
									  errmsg(ERRMSG_JSON_MEMBER_NOT_FOUND),
									  errdetail("jsonpath member accessor can "
												"only be applied to an object"))));
			}
			break;

		case jpiRoot:
			jb = cxt->root;
			baseObject = setBaseObject(cxt, jb, 0);
			res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			cxt->baseObject = baseObject;
			break;

		case jpiCurrent:
			res = executeNextItem(cxt, jsp, NULL, cxt->stack->item,
								  found, true);
			break;

		case jpiAnyArray:
			if (JsonbType(jb) == jbvArray)
			{
				bool		hasNext = jspGetNext(jsp, &elem);

				res = executeItemUnwrapTargetArray(cxt, hasNext ? &elem : NULL,
												   jb, found, jspAutoUnwrap(cxt));
			}
			else if (jspAutoWrap(cxt))
				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			else if (!jspIgnoreStructuralErrors(cxt))
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_JSON_ARRAY_NOT_FOUND),
									  errmsg(ERRMSG_JSON_ARRAY_NOT_FOUND),
									  errdetail("jsonpath wildcard array accessor "
												"can only be applied to an array"))));
			break;

		case jpiIndexArray:
			if (JsonbType(jb) == jbvArray || jspAutoWrap(cxt))
			{
				int			innermostArraySize = cxt->innermostArraySize;
				int			i;
				int			size = JsonbArraySize(jb);
				bool		singleton = size < 0;
				bool		hasNext = jspGetNext(jsp, &elem);

				if (singleton)
					size = 1;

				cxt->innermostArraySize = size; /* for LAST evaluation */

				for (i = 0; i < jsp->content.array.nelems; i++)
				{
					JsonPathItem from;
					JsonPathItem to;
					int32		index;
					int32		index_from;
					int32		index_to;
					bool		range = jspGetArraySubscript(jsp, &from,
															 &to, i);

					res = getArrayIndex(cxt, &from, jb, &index_from);

					if (jperIsError(res))
						break;

					if (range)
					{
						res = getArrayIndex(cxt, &to, jb, &index_to);

						if (jperIsError(res))
							break;
					}
					else
						index_to = index_from;

					if (!jspIgnoreStructuralErrors(cxt) &&
						(index_from < 0 ||
						 index_from > index_to ||
						 index_to >= size))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_JSON_SUBSCRIPT),
											  errmsg(ERRMSG_INVALID_JSON_SUBSCRIPT),
											  errdetail("jsonpath array subscript is "
														"out of bounds"))));

					if (index_from < 0)
						index_from = 0;

					if (index_to >= size)
						index_to = size - 1;

					res = jperNotFound;

					for (index = index_from; index <= index_to; index++)
					{
						JsonItem   *jsi;
						bool		copy;

						if (singleton)
						{
							jsi = jb;
							copy = true;
						}
						else
						{
							JsonbValue *v =
							getIthJsonbValueFromContainer(jb->jbv.val.binary.data,
														  (uint32) index);

							if (v == NULL)
								continue;

							jsi = JsonbValueToJsonItem(v);
							copy = false;
						}

						if (!hasNext && !found)
							return jperOk;

						res = executeNextItem(cxt, jsp, &elem, jsi, found,
											  copy);

						if (jperIsError(res))
							break;

						if (res == jperOk && !found)
							break;
					}

					if (jperIsError(res))
						break;

					if (res == jperOk && !found)
						break;
				}

				cxt->innermostArraySize = innermostArraySize;
			}
			else if (!jspIgnoreStructuralErrors(cxt))
			{
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_JSON_ARRAY_NOT_FOUND),
									  errmsg(ERRMSG_JSON_ARRAY_NOT_FOUND),
									  errdetail("jsonpath array accessor can "
												"only be applied to an array"))));
			}
			break;

		case jpiLast:
			{
				JsonItem	tmpjsi;
				JsonItem   *lastjsi;
				int			last;
				bool		hasNext = jspGetNext(jsp, &elem);

				if (cxt->innermostArraySize < 0)
					elog(ERROR, "evaluating jsonpath LAST outside of "
						 "array subscript");

				if (!hasNext && !found)
				{
					res = jperOk;
					break;
				}

				last = cxt->innermostArraySize - 1;

				lastjsi = hasNext ? &tmpjsi : palloc(sizeof(*lastjsi));

				lastjsi->jbv.type = jbvNumeric;
				lastjsi->jbv.val.numeric =
					DatumGetNumeric(DirectFunctionCall1(int4_numeric,
														Int32GetDatum(last)));

				res = executeNextItem(cxt, jsp, &elem, lastjsi, found, hasNext);
			}
			break;

		case jpiAnyKey:
			if (JsonbType(jb) == jbvObject)
			{
				bool		hasNext = jspGetNext(jsp, &elem);

				if (jb->type != jbvBinary)
					elog(ERROR, "invalid jsonb object type: %d", jb->type);

				return executeAnyItem
					(cxt, hasNext ? &elem : NULL,
					 jb->jbv.val.binary.data, found, 1, 1, 1,
					 false, jspAutoUnwrap(cxt));
			}
			else if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);
			else if (!jspIgnoreStructuralErrors(cxt))
			{
				Assert(found);
				RETURN_ERROR(ereport(ERROR,
									 (errcode(ERRCODE_JSON_OBJECT_NOT_FOUND),
									  errmsg(ERRMSG_JSON_OBJECT_NOT_FOUND),
									  errdetail("jsonpath wildcard member accessor "
												"can only be applied to an object"))));
			}
			break;

		case jpiAdd:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_add_opt_error, found);

		case jpiSub:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_sub_opt_error, found);

		case jpiMul:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_mul_opt_error, found);

		case jpiDiv:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_div_opt_error, found);

		case jpiMod:
			return executeBinaryArithmExpr(cxt, jsp, jb,
										   numeric_mod_opt_error, found);

		case jpiPlus:
			return executeUnaryArithmExpr(cxt, jsp, jb, NULL, found);

		case jpiMinus:
			return executeUnaryArithmExpr(cxt, jsp, jb, numeric_uminus,
										  found);

		case jpiFilter:
			{
				JsonPathBool st;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				jspGetArg(jsp, &elem);
				st = executeNestedBoolItem(cxt, &elem, jb);
				if (st != jpbTrue)
					res = jperNotFound;
				else
					res = executeNextItem(cxt, jsp, NULL,
										  jb, found, true);
				break;
			}

		case jpiAny:
			{
				bool		hasNext = jspGetNext(jsp, &elem);

				/* first try without any intermediate steps */
				if (jsp->content.anybounds.first == 0)
				{
					bool		savedIgnoreStructuralErrors;

					savedIgnoreStructuralErrors = cxt->ignoreStructuralErrors;
					cxt->ignoreStructuralErrors = true;
					res = executeNextItem(cxt, jsp, &elem,
										  jb, found, true);
					cxt->ignoreStructuralErrors = savedIgnoreStructuralErrors;

					if (res == jperOk && !found)
						break;
				}

				if (jb->type == jbvBinary)
					res = executeAnyItem
						(cxt, hasNext ? &elem : NULL,
						 jb->jbv.val.binary.data, found,
						 1,
						 jsp->content.anybounds.first,
						 jsp->content.anybounds.last,
						 true, jspAutoUnwrap(cxt));
				break;
			}

		case jpiNull:
		case jpiBool:
		case jpiNumeric:
		case jpiString:
		case jpiVariable:
			{
				JsonItem	vbuf;
				JsonItem   *v;
				bool		hasNext = jspGetNext(jsp, &elem);

				if (!hasNext && !found)
				{
					res = jperOk;	/* skip evaluation */
					break;
				}

				v = hasNext ? &vbuf : palloc(sizeof(*v));

				baseObject = cxt->baseObject;
				getJsonPathItem(cxt, jsp, v);

				res = executeNextItem(cxt, jsp, &elem,
									  v, found, hasNext);
				cxt->baseObject = baseObject;
			}
			break;

		case jpiType:
			{
				JsonItem	jsi;

				jsi.jbv.type = jbvString;
				jsi.jbv.val.string.val = pstrdup(JsonItemTypeName(jb));
				jsi.jbv.val.string.len = strlen(jsi.jbv.val.string.val);

				res = executeNextItem(cxt, jsp, NULL, &jsi, found, true);
			}
			break;

		case jpiSize:
			{
				int			size = JsonbArraySize(jb);

				if (size < 0)
				{
					if (!jspAutoWrap(cxt))
					{
						if (!jspIgnoreStructuralErrors(cxt))
							RETURN_ERROR(ereport(ERROR,
												 (errcode(ERRCODE_JSON_ARRAY_NOT_FOUND),
												  errmsg(ERRMSG_JSON_ARRAY_NOT_FOUND),
												  errdetail("jsonpath item method .%s() "
															"can only be applied to an array",
															jspOperationName(jsp->type)))));
						break;
					}

					size = 1;
				}

				jb = palloc(sizeof(*jb));

				jb->jbv.type = jbvNumeric;
				jb->jbv.val.numeric =
					DatumGetNumeric(DirectFunctionCall1(int4_numeric,
														Int32GetDatum(size)));

				res = executeNextItem(cxt, jsp, NULL, jb, found, false);
			}
			break;

		case jpiAbs:
			return executeNumericItemMethod(cxt, jsp, jb, unwrap, numeric_abs,
											found);

		case jpiFloor:
			return executeNumericItemMethod(cxt, jsp, jb, unwrap, numeric_floor,
											found);

		case jpiCeiling:
			return executeNumericItemMethod(cxt, jsp, jb, unwrap, numeric_ceil,
											found);

		case jpiDouble:
			{
				JsonItem	jsi;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				if (jb->type == jbvNumeric)
				{
					char	   *tmp = DatumGetCString(DirectFunctionCall1(numeric_out,
																		  NumericGetDatum(jb->jbv.val.numeric)));
					bool		have_error = false;

					(void) float8in_internal_opt_error(tmp,
													   NULL,
													   "double precision",
													   tmp,
													   &have_error);

					if (have_error)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_JSON_ITEM),
											  errmsg(ERRMSG_NON_NUMERIC_JSON_ITEM),
											  errdetail("jsonpath item method .%s() "
														"can only be applied to "
														"a numeric value",
														jspOperationName(jsp->type)))));
					res = jperOk;
				}
				else if (jb->type == jbvString)
				{
					/* cast string as double */
					double		val;
					char	   *tmp = pnstrdup(jb->jbv.val.string.val,
											   jb->jbv.val.string.len);
					bool		have_error = false;

					val = float8in_internal_opt_error(tmp,
													  NULL,
													  "double precision",
													  tmp,
													  &have_error);

					if (have_error || isinf(val))
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_NON_NUMERIC_JSON_ITEM),
											  errmsg(ERRMSG_NON_NUMERIC_JSON_ITEM),
											  errdetail("jsonpath item method .%s() can "
														"only be applied to a numeric value",
														jspOperationName(jsp->type)))));

					jb = &jsi;
					jb->jbv.type = jbvNumeric;
					jb->jbv.val.numeric = DatumGetNumeric(DirectFunctionCall1(float8_numeric,
																			  Float8GetDatum(val)));
					res = jperOk;
				}

				if (res == jperNotFound)
					RETURN_ERROR(ereport(ERROR,
										 (errcode(ERRCODE_NON_NUMERIC_JSON_ITEM),
										  errmsg(ERRMSG_NON_NUMERIC_JSON_ITEM),
										  errdetail("jsonpath item method .%s() "
													"can only be applied to a "
													"string or numeric value",
													jspOperationName(jsp->type)))));

				res = executeNextItem(cxt, jsp, NULL, jb, found, true);
			}
			break;

		case jpiDatetime:
			{
				JsonbValue	jbvbuf;
				Datum		value;
				text	   *datetime;
				Oid			typid;
				int32		typmod = -1;
				int			tz = PG_INT32_MIN;
				bool		hasNext;
				char	   *tzname = NULL;

				if (unwrap && JsonbType(jb) == jbvArray)
					return executeItemUnwrapTargetArray(cxt, jsp, jb, found,
														false);

				if (!(jb = getScalar(jb, jbvString)))
					RETURN_ERROR(ereport(ERROR,
										 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_JSON_DATETIME_FUNCTION),
										  errmsg(ERRMSG_INVALID_ARGUMENT_FOR_JSON_DATETIME_FUNCTION),
										  errdetail("jsonpath item method .%s() is "
													"applied to not a string",
													jspOperationName(jsp->type)))));

				datetime = cstring_to_text_with_len(jb->jbv.val.string.val,
													jb->jbv.val.string.len);

				if (jsp->content.args.left)
				{
					text	   *template;
					char	   *template_str;
					int			template_len;

					jspGetLeftArg(jsp, &elem);

					if (elem.type != jpiString)
						elog(ERROR, "invalid jsonpath item type for "
							 ".datetime() argument");

					template_str = jspGetString(&elem, &template_len);

					if (jsp->content.args.right)
					{
						JsonValueList tzlist = {0};
						JsonPathExecResult tzres;
						JsonItem   *tzjsi;

						jspGetRightArg(jsp, &elem);
						tzres = executeItem(cxt, &elem, jb, &tzlist);
						if (jperIsError(tzres))
							return tzres;

						if (JsonValueListLength(&tzlist) != 1 ||
							((tzjsi = JsonValueListHead(&tzlist))->type != jbvString &&
							 tzjsi->type != jbvNumeric))
							RETURN_ERROR(ereport(ERROR,
												 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_JSON_DATETIME_FUNCTION),
												  errmsg(ERRMSG_INVALID_ARGUMENT_FOR_JSON_DATETIME_FUNCTION),
												  errdetail("timezone argument of "
															"jsonpath item method .%s() "
															"is not a singleton string or number",
															jspOperationName(jsp->type)))));

						if (tzjsi->type == jbvString)
							tzname = pnstrdup(tzjsi->jbv.val.string.val,
											  tzjsi->jbv.val.string.len);
						else
						{
							bool		error = false;

							tz = numeric_int4_opt_error(tzjsi->jbv.val.numeric,
														&error);

							if (error || tz == PG_INT32_MIN)
								RETURN_ERROR(ereport(ERROR,
													 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_JSON_DATETIME_FUNCTION),
													  errmsg(ERRMSG_INVALID_ARGUMENT_FOR_JSON_DATETIME_FUNCTION),
													  errdetail("timezone argument of "
																"jsonpath item method .%s() "
																"is out of integer range",
																jspOperationName(jsp->type)))));

							tz = -tz;
						}
					}

					if (template_len)
					{
						template = cstring_to_text_with_len(template_str,
															template_len);

						if (tryToParseDatetime(template, datetime, tzname, false,
											   &value, &typid, &typmod,
											   &tz, jspThrowErrors(cxt)))
							res = jperOk;
						else
							res = jperError;
					}
				}

				if (res == jperNotFound)
				{
					/* Try to recognize one of ISO formats. */
					static const char *fmt_str[] =
					{
						"yyyy-mm-dd HH24:MI:SS TZH:TZM",
						"yyyy-mm-dd HH24:MI:SS TZH",
						"yyyy-mm-dd HH24:MI:SS",
						"yyyy-mm-dd",
						"HH24:MI:SS TZH:TZM",
						"HH24:MI:SS TZH",
						"HH24:MI:SS"
					};

					/* cache for format texts */
					static text *fmt_txt[lengthof(fmt_str)] = {0};
					int			i;

					for (i = 0; i < lengthof(fmt_str); i++)
					{
						if (!fmt_txt[i])
						{
							MemoryContext oldcxt =
							MemoryContextSwitchTo(TopMemoryContext);

							fmt_txt[i] = cstring_to_text(fmt_str[i]);
							MemoryContextSwitchTo(oldcxt);
						}

						if (tryToParseDatetime(fmt_txt[i], datetime, tzname,
											   true, &value, &typid, &typmod,
											   &tz, false))
						{
							res = jperOk;
							break;
						}
					}

					if (res == jperNotFound)
						RETURN_ERROR(ereport(ERROR,
											 (errcode(ERRCODE_INVALID_ARGUMENT_FOR_JSON_DATETIME_FUNCTION),
											  errmsg(ERRMSG_INVALID_ARGUMENT_FOR_JSON_DATETIME_FUNCTION),
											  errdetail("unrecognized datetime format"),
											  errhint("use datetime template "
													  "argument for explicit "
													  "format specification"))));
				}

				if (tzname)
					pfree(tzname);

				pfree(datetime);

				if (jperIsError(res))
					break;

				hasNext = jspGetNext(jsp, &elem);

				if (!hasNext && !found)
					break;

				jb = hasNext ? &jbvbuf : palloc(sizeof(*jb));

				jb->type = jsiDatetime;
				jb->datetime.value = value;
				jb->datetime.typid = typid;
				jb->datetime.typmod = typmod;
				jb->datetime.tz = tz;

				res = executeNextItem(cxt, jsp, &elem, jb, found, hasNext);
			}
			break;

		case jpiKeyValue:
			if (unwrap && JsonbType(jb) == jbvArray)
				return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);

			return executeKeyValueMethod(cxt, jsp, jb, found);

		default:
			elog(ERROR, "unrecognized jsonpath item type: %d", jsp->type);
	}

	return res;
}

/*
 * Unwrap current array item and execute jsonpath for each of its elements.
 */
static JsonPathExecResult
executeItemUnwrapTargetArray(JsonPathExecContext *cxt, JsonPathItem *jsp,
							 JsonItem *jb, JsonValueList *found,
							 bool unwrapElements)
{
	if (jb->type != jbvBinary)
	{
		Assert(jb->type != jbvArray);
		elog(ERROR, "invalid jsonb array value type: %d", jb->type);
	}

	return executeAnyItem
		(cxt, jsp, jb->jbv.val.binary.data, found, 1, 1, 1,
		 false, unwrapElements);
}

/*
 * Execute next jsonpath item if exists.  Otherwise put "v" to the "found"
 * list if provided.
 */
static JsonPathExecResult
executeNextItem(JsonPathExecContext *cxt,
				JsonPathItem *cur, JsonPathItem *next,
				JsonItem *v, JsonValueList *found, bool copy)
{
	JsonPathItem elem;
	bool		hasNext;

	if (!cur)
		hasNext = next != NULL;
	else if (next)
		hasNext = jspHasNext(cur);
	else
	{
		next = &elem;
		hasNext = jspGetNext(cur, next);
	}

	if (hasNext)
		return executeItem(cxt, next, v, found);

	if (found)
		JsonValueListAppend(found, copy ? copyJsonItem(v) : v);

	return jperOk;
}

/*
 * Same as executeItem(), but when "unwrap == true" automatically unwraps
 * each array item from the resulting sequence in lax mode.
 */
static JsonPathExecResult
executeItemOptUnwrapResult(JsonPathExecContext *cxt, JsonPathItem *jsp,
						   JsonItem *jb, bool unwrap,
						   JsonValueList *found)
{
	if (unwrap && jspAutoUnwrap(cxt))
	{
		JsonValueList seq = {0};
		JsonValueListIterator it;
		JsonPathExecResult res = executeItem(cxt, jsp, jb, &seq);
		JsonItem   *item;

		if (jperIsError(res))
			return res;

		JsonValueListInitIterator(&seq, &it);
		while ((item = JsonValueListNext(&seq, &it)))
		{
			Assert(item->type != jbvArray);

			if (JsonbType(item) == jbvArray)
				executeItemUnwrapTargetArray(cxt, NULL, item, found, false);
			else
				JsonValueListAppend(found, item);
		}

		return jperOk;
	}

	return executeItem(cxt, jsp, jb, found);
}

/*
 * Same as executeItemOptUnwrapResult(), but with error suppression.
 */
static JsonPathExecResult
executeItemOptUnwrapResultNoThrow(JsonPathExecContext *cxt,
								  JsonPathItem *jsp,
								  JsonItem *jb, bool unwrap,
								  JsonValueList *found)
{
	JsonPathExecResult res;
	bool		throwErrors = cxt->throwErrors;

	cxt->throwErrors = false;
	res = executeItemOptUnwrapResult(cxt, jsp, jb, unwrap, found);
	cxt->throwErrors = throwErrors;

	return res;
}

/* Execute boolean-valued jsonpath expression. */
static JsonPathBool
executeBoolItem(JsonPathExecContext *cxt, JsonPathItem *jsp,
				JsonItem *jb, bool canHaveNext)
{
	JsonPathItem larg;
	JsonPathItem rarg;
	JsonPathBool res;
	JsonPathBool res2;

	if (!canHaveNext && jspHasNext(jsp))
		elog(ERROR, "boolean jsonpath item cannot have next item");

	switch (jsp->type)
	{
		case jpiAnd:
			jspGetLeftArg(jsp, &larg);
			res = executeBoolItem(cxt, &larg, jb, false);

			if (res == jpbFalse)
				return jpbFalse;

			/*
			 * SQL/JSON says that we should check second arg in case of
			 * jperError
			 */

			jspGetRightArg(jsp, &rarg);
			res2 = executeBoolItem(cxt, &rarg, jb, false);

			return res2 == jpbTrue ? res : res2;

		case jpiOr:
			jspGetLeftArg(jsp, &larg);
			res = executeBoolItem(cxt, &larg, jb, false);

			if (res == jpbTrue)
				return jpbTrue;

			jspGetRightArg(jsp, &rarg);
			res2 = executeBoolItem(cxt, &rarg, jb, false);

			return res2 == jpbFalse ? res : res2;

		case jpiNot:
			jspGetArg(jsp, &larg);

			res = executeBoolItem(cxt, &larg, jb, false);

			if (res == jpbUnknown)
				return jpbUnknown;

			return res == jpbTrue ? jpbFalse : jpbTrue;

		case jpiIsUnknown:
			jspGetArg(jsp, &larg);
			res = executeBoolItem(cxt, &larg, jb, false);
			return res == jpbUnknown ? jpbTrue : jpbFalse;

		case jpiEqual:
		case jpiNotEqual:
		case jpiLess:
		case jpiGreater:
		case jpiLessOrEqual:
		case jpiGreaterOrEqual:
			jspGetLeftArg(jsp, &larg);
			jspGetRightArg(jsp, &rarg);
			return executePredicate(cxt, jsp, &larg, &rarg, jb, true,
									executeComparison, NULL);

		case jpiStartsWith:		/* 'whole STARTS WITH initial' */
			jspGetLeftArg(jsp, &larg);	/* 'whole' */
			jspGetRightArg(jsp, &rarg); /* 'initial' */
			return executePredicate(cxt, jsp, &larg, &rarg, jb, false,
									executeStartsWith, NULL);

		case jpiLikeRegex:		/* 'expr LIKE_REGEX pattern FLAGS flags' */
			{
				/*
				 * 'expr' is a sequence-returning expression.  'pattern' is a
				 * regex string literal.  SQL/JSON standard requires XQuery
				 * regexes, but we use Postgres regexes here.  'flags' is a
				 * string literal converted to integer flags at compile-time.
				 */
				JsonLikeRegexContext lrcxt = {0};

				jspInitByBuffer(&larg, jsp->base,
								jsp->content.like_regex.expr);

				return executePredicate(cxt, jsp, &larg, NULL, jb, false,
										executeLikeRegex, &lrcxt);
			}

		case jpiExists:
			jspGetArg(jsp, &larg);

			if (jspStrictAbsenseOfErrors(cxt))
			{
				/*
				 * In strict mode we must get a complete list of values to
				 * check that there are no errors at all.
				 */
				JsonValueList vals = {0};
				JsonPathExecResult res =
				executeItemOptUnwrapResultNoThrow(cxt, &larg, jb,
												  false, &vals);

				if (jperIsError(res))
					return jpbUnknown;

				return JsonValueListIsEmpty(&vals) ? jpbFalse : jpbTrue;
			}
			else
			{
				JsonPathExecResult res =
				executeItemOptUnwrapResultNoThrow(cxt, &larg, jb,
												  false, NULL);

				if (jperIsError(res))
					return jpbUnknown;

				return res == jperOk ? jpbTrue : jpbFalse;
			}

		default:
			elog(ERROR, "invalid boolean jsonpath item type: %d", jsp->type);
			return jpbUnknown;
	}
}

/*
 * Execute nested (filters etc.) boolean expression pushing current SQL/JSON
 * item onto the stack.
 */
static JsonPathBool
executeNestedBoolItem(JsonPathExecContext *cxt, JsonPathItem *jsp,
					  JsonItem *jb)
{
	JsonItemStackEntry current;
	JsonPathBool res;

	pushJsonItem(&cxt->stack, &current, jb);
	res = executeBoolItem(cxt, jsp, jb, false);
	popJsonItem(&cxt->stack);

	return res;
}

/*
 * Implementation of several jsonpath nodes:
 *  - jpiAny (.** accessor),
 *  - jpiAnyKey (.* accessor),
 *  - jpiAnyArray ([*] accessor)
 */
static JsonPathExecResult
executeAnyItem(JsonPathExecContext *cxt, JsonPathItem *jsp, JsonbContainer *jbc,
			   JsonValueList *found, uint32 level, uint32 first, uint32 last,
			   bool ignoreStructuralErrors, bool unwrapNext)
{
	JsonPathExecResult res = jperNotFound;
	JsonbIterator *it;
	int32		r;
	JsonItem	v;

	check_stack_depth();

	if (level > last)
		return res;

	it = JsonbIteratorInit(jbc);

	/*
	 * Recursively iterate over jsonb objects/arrays
	 */
	while ((r = JsonbIteratorNext(&it, &v.jbv, true)) != WJB_DONE)
	{
		if (r == WJB_KEY)
		{
			r = JsonbIteratorNext(&it, &v.jbv, true);
			Assert(r == WJB_VALUE);
		}

		if (r == WJB_VALUE || r == WJB_ELEM)
		{

			if (level >= first ||
				(first == PG_UINT32_MAX && last == PG_UINT32_MAX &&
				 v.type != jbvBinary))	/* leaves only requested */
			{
				/* check expression */
				if (jsp)
				{
					if (ignoreStructuralErrors)
					{
						bool		savedIgnoreStructuralErrors;

						savedIgnoreStructuralErrors = cxt->ignoreStructuralErrors;
						cxt->ignoreStructuralErrors = true;
						res = executeItemOptUnwrapTarget(cxt, jsp, &v, found, unwrapNext);
						cxt->ignoreStructuralErrors = savedIgnoreStructuralErrors;
					}
					else
						res = executeItemOptUnwrapTarget(cxt, jsp, &v, found, unwrapNext);

					if (jperIsError(res))
						break;

					if (res == jperOk && !found)
						break;
				}
				else if (found)
					JsonValueListAppend(found, copyJsonItem(&v));
				else
					return jperOk;
			}

			if (level < last && v.type == jbvBinary)
			{
				res = executeAnyItem
					(cxt, jsp, v.jbv.val.binary.data, found,
					 level + 1, first, last,
					 ignoreStructuralErrors, unwrapNext);

				if (jperIsError(res))
					break;

				if (res == jperOk && found == NULL)
					break;
			}
		}
	}

	return res;
}

/*
 * Execute unary or binary predicate.
 *
 * Predicates have existence semantics, because their operands are item
 * sequences.  Pairs of items from the left and right operand's sequences are
 * checked.  TRUE returned only if any pair satisfying the condition is found.
 * In strict mode, even if the desired pair has already been found, all pairs
 * still need to be examined to check the absence of errors.  If any error
 * occurs, UNKNOWN (analogous to SQL NULL) is returned.
 */
static JsonPathBool
executePredicate(JsonPathExecContext *cxt, JsonPathItem *pred,
				 JsonPathItem *larg, JsonPathItem *rarg, JsonItem *jb,
				 bool unwrapRightArg, JsonPathPredicateCallback exec,
				 void *param)
{
	JsonPathExecResult res;
	JsonValueListIterator lseqit;
	JsonValueList lseq = {0};
	JsonValueList rseq = {0};
	JsonItem *lval;
	bool		error = false;
	bool		found = false;

	/* Left argument is always auto-unwrapped. */
	res = executeItemOptUnwrapResultNoThrow(cxt, larg, jb, true, &lseq);
	if (jperIsError(res))
		return jpbUnknown;

	if (rarg)
	{
		/* Right argument is conditionally auto-unwrapped. */
		res = executeItemOptUnwrapResultNoThrow(cxt, rarg, jb,
												unwrapRightArg, &rseq);
		if (jperIsError(res))
			return jpbUnknown;
	}

	JsonValueListInitIterator(&lseq, &lseqit);
	while ((lval = JsonValueListNext(&lseq, &lseqit)))
	{
		JsonValueListIterator rseqit;
		JsonItem   *rval;
		bool		first = true;

		JsonValueListInitIterator(&rseq, &rseqit);
		if (rarg)
			rval = JsonValueListNext(&rseq, &rseqit);
		else
			rval = NULL;

		/* Loop over right arg sequence or do single pass otherwise */
		while (rarg ? (rval != NULL) : first)
		{
			JsonPathBool res = exec(pred, lval, rval, param);

			if (res == jpbUnknown)
			{
				if (jspStrictAbsenseOfErrors(cxt))
					return jpbUnknown;

				error = true;
			}
			else if (res == jpbTrue)
			{
				if (!jspStrictAbsenseOfErrors(cxt))
					return jpbTrue;

				found = true;
			}

			first = false;
			if (rarg)
				rval = JsonValueListNext(&rseq, &rseqit);
		}
	}

	if (found)					/* possible only in strict mode */
		return jpbTrue;

	if (error)					/* possible only in lax mode */
		return jpbUnknown;

	return jpbFalse;
}

/*
 * Execute binary arithmetic expression on singleton numeric operands.
 * Array operands are automatically unwrapped in lax mode.
 */
static JsonPathExecResult
executeBinaryArithmExpr(JsonPathExecContext *cxt, JsonPathItem *jsp,
						JsonItem *jb, BinaryArithmFunc func,
						JsonValueList *found)
{
	JsonPathExecResult jper;
	JsonPathItem elem;
	JsonValueList lseq = {0};
	JsonValueList rseq = {0};
	JsonItem   *lval;
	JsonItem   *rval;
	Numeric		res;

	jspGetLeftArg(jsp, &elem);

	/*
	 * XXX: By standard only operands of multiplicative expressions are
	 * unwrapped.  We extend it to other binary arithmetics expressions too.
	 */
	jper = executeItemOptUnwrapResult(cxt, &elem, jb, true, &lseq);
	if (jperIsError(jper))
		return jper;

	jspGetRightArg(jsp, &elem);

	jper = executeItemOptUnwrapResult(cxt, &elem, jb, true, &rseq);
	if (jperIsError(jper))
		return jper;

	if (JsonValueListLength(&lseq) != 1 ||
		!(lval = getScalar(JsonValueListHead(&lseq), jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_SINGLETON_JSON_ITEM_REQUIRED),
							  errmsg(ERRMSG_SINGLETON_JSON_ITEM_REQUIRED),
							  errdetail("left operand of binary jsonpath operator %s "
										"is not a singleton numeric value",
										jspOperationName(jsp->type)))));

	if (JsonValueListLength(&rseq) != 1 ||
		!(rval = getScalar(JsonValueListHead(&rseq), jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_SINGLETON_JSON_ITEM_REQUIRED),
							  errmsg(ERRMSG_SINGLETON_JSON_ITEM_REQUIRED),
							  errdetail("right operand of binary jsonpath operator %s "
										"is not a singleton numeric value",
										jspOperationName(jsp->type)))));

	if (jspThrowErrors(cxt))
	{
		res = func(lval->jbv.val.numeric, rval->jbv.val.numeric, NULL);
	}
	else
	{
		bool		error = false;

		res = func(lval->jbv.val.numeric, rval->jbv.val.numeric, &error);

		if (error)
			return jperError;
	}

	if (!jspGetNext(jsp, &elem) && !found)
		return jperOk;

	lval = palloc(sizeof(*lval));
	lval->jbv.type = jbvNumeric;
	lval->jbv.val.numeric = res;

	return executeNextItem(cxt, jsp, &elem, lval, found, false);
}

/*
 * Execute unary arithmetic expression for each numeric item in its operand's
 * sequence.  Array operand is automatically unwrapped in lax mode.
 */
static JsonPathExecResult
executeUnaryArithmExpr(JsonPathExecContext *cxt, JsonPathItem *jsp,
					   JsonItem *jb, PGFunction func, JsonValueList *found)
{
	JsonPathExecResult jper;
	JsonPathExecResult jper2;
	JsonPathItem elem;
	JsonValueList seq = {0};
	JsonValueListIterator it;
	JsonItem   *val;
	bool		hasNext;

	jspGetArg(jsp, &elem);
	jper = executeItemOptUnwrapResult(cxt, &elem, jb, true, &seq);

	if (jperIsError(jper))
		return jper;

	jper = jperNotFound;

	hasNext = jspGetNext(jsp, &elem);

	JsonValueListInitIterator(&seq, &it);
	while ((val = JsonValueListNext(&seq, &it)))
	{
		if ((val = getScalar(val, jbvNumeric)))
		{
			if (!found && !hasNext)
				return jperOk;
		}
		else
		{
			if (!found && !hasNext)
				continue;		/* skip non-numerics processing */

			RETURN_ERROR(ereport(ERROR,
								 (errcode(ERRCODE_JSON_NUMBER_NOT_FOUND),
								  errmsg(ERRMSG_JSON_NUMBER_NOT_FOUND),
								  errdetail("operand of unary jsonpath operator %s "
											"is not a numeric value",
											jspOperationName(jsp->type)))));
		}

		if (func)
			val->jbv.val.numeric =
				DatumGetNumeric(DirectFunctionCall1(func,
													NumericGetDatum(val->jbv.val.numeric)));

		jper2 = executeNextItem(cxt, jsp, &elem, val, found, false);

		if (jperIsError(jper2))
			return jper2;

		if (jper2 == jperOk)
		{
			if (!found)
				return jperOk;
			jper = jperOk;
		}
	}

	return jper;
}

/*
 * STARTS_WITH predicate callback.
 *
 * Check if the 'whole' string starts from 'initial' string.
 */
static JsonPathBool
executeStartsWith(JsonPathItem *jsp, JsonItem *whole, JsonItem *initial,
				  void *param)
{
	if (!(whole = getScalar(whole, jbvString)))
		return jpbUnknown;		/* error */

	if (!(initial = getScalar(initial, jbvString)))
		return jpbUnknown;		/* error */

	if (whole->jbv.val.string.len >= initial->jbv.val.string.len &&
		!memcmp(whole->jbv.val.string.val,
				initial->jbv.val.string.val,
				initial->jbv.val.string.len))
		return jpbTrue;

	return jpbFalse;
}

/*
 * LIKE_REGEX predicate callback.
 *
 * Check if the string matches regex pattern.
 */
static JsonPathBool
executeLikeRegex(JsonPathItem *jsp, JsonItem *str, JsonItem *rarg,
				 void *param)
{
	JsonLikeRegexContext *cxt = param;

	if (!(str = getScalar(str, jbvString)))
		return jpbUnknown;

	/* Cache regex text and converted flags. */
	if (!cxt->regex)
	{
		uint32		flags = jsp->content.like_regex.flags;

		cxt->regex =
			cstring_to_text_with_len(jsp->content.like_regex.pattern,
									 jsp->content.like_regex.patternlen);

		/* Convert regex flags. */
		cxt->cflags = REG_ADVANCED;

		if (flags & JSP_REGEX_ICASE)
			cxt->cflags |= REG_ICASE;
		if (flags & JSP_REGEX_MLINE)
			cxt->cflags |= REG_NEWLINE;
		if (flags & JSP_REGEX_SLINE)
			cxt->cflags &= ~REG_NEWLINE;
		if (flags & JSP_REGEX_WSPACE)
			cxt->cflags |= REG_EXPANDED;
	}

	if (RE_compile_and_execute(cxt->regex, str->jbv.val.string.val,
							   str->jbv.val.string.len,
							   cxt->cflags, DEFAULT_COLLATION_OID, 0, NULL))
		return jpbTrue;

	return jpbFalse;
}

/*
 * Execute numeric item methods (.abs(), .floor(), .ceil()) using the specified
 * user function 'func'.
 */
static JsonPathExecResult
executeNumericItemMethod(JsonPathExecContext *cxt, JsonPathItem *jsp,
						 JsonItem *jb, bool unwrap, PGFunction func,
						 JsonValueList *found)
{
	JsonPathItem next;
	Datum		datum;

	if (unwrap && JsonbType(jb) == jbvArray)
		return executeItemUnwrapTargetArray(cxt, jsp, jb, found, false);

	if (!(jb = getScalar(jb, jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_NON_NUMERIC_JSON_ITEM),
							  errmsg(ERRMSG_NON_NUMERIC_JSON_ITEM),
							  errdetail("jsonpath item method .%s() can only "
										"be applied to a numeric value",
										jspOperationName(jsp->type)))));

	datum = DirectFunctionCall1(func, NumericGetDatum(jb->jbv.val.numeric));

	if (!jspGetNext(jsp, &next) && !found)
		return jperOk;

	jb = palloc(sizeof(*jb));
	jb->jbv.type = jbvNumeric;
	jb->jbv.val.numeric = DatumGetNumeric(datum);

	return executeNextItem(cxt, jsp, &next, jb, found, false);
}

/*
 * Implementation of .keyvalue() method.
 *
 * .keyvalue() method returns a sequence of object's key-value pairs in the
 * following format: '{ "key": key, "value": value, "id": id }'.
 *
 * "id" field is an object identifier which is constructed from the two parts:
 * base object id and its binary offset in base object's jsonb:
 * id = 10000000000 * base_object_id + obj_offset_in_base_object
 *
 * 10000000000 (10^10) -- is a first round decimal number greater than 2^32
 * (maximal offset in jsonb).  Decimal multiplier is used here to improve the
 * readability of identifiers.
 *
 * Base object is usually a root object of the path: context item '$' or path
 * variable '$var', literals can't produce objects for now.  But if the path
 * contains generated objects (.keyvalue() itself, for example), then they
 * become base object for the subsequent .keyvalue().
 *
 * Id of '$' is 0. Id of '$var' is its ordinal (positive) number in the list
 * of variables (see getJsonPathVariable()).  Ids for generated objects
 * are assigned using global counter JsonPathExecContext.lastGeneratedObjectId.
 */
static JsonPathExecResult
executeKeyValueMethod(JsonPathExecContext *cxt, JsonPathItem *jsp,
					  JsonItem *jb, JsonValueList *found)
{
	JsonPathExecResult res = jperNotFound;
	JsonPathItem next;
	JsonbContainer *jbc;
	JsonbValue	key;
	JsonbValue	val;
	JsonbValue	idval;
	JsonbValue	keystr;
	JsonbValue	valstr;
	JsonbValue	idstr;
	JsonbIterator *it;
	JsonbIteratorToken tok;
	int64		id;
	bool		hasNext;

	if (JsonbType(jb) != jbvObject || jb->type != jbvBinary)
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_JSON_OBJECT_NOT_FOUND),
							  errmsg(ERRMSG_JSON_OBJECT_NOT_FOUND),
							  errdetail("jsonpath item method .%s() "
										"can only be applied to an object",
										jspOperationName(jsp->type)))));

	jbc = jb->jbv.val.binary.data;

	if (!JsonContainerSize(jbc))
		return jperNotFound;	/* no key-value pairs */

	hasNext = jspGetNext(jsp, &next);

	keystr.type = jbvString;
	keystr.val.string.val = "key";
	keystr.val.string.len = 3;

	valstr.type = jbvString;
	valstr.val.string.val = "value";
	valstr.val.string.len = 5;

	idstr.type = jbvString;
	idstr.val.string.val = "id";
	idstr.val.string.len = 2;

	/* construct object id from its base object and offset inside that */
	id = jb->type != jbvBinary ? 0 :
		(int64) ((char *) jbc - (char *) cxt->baseObject.jbc);
	id += (int64) cxt->baseObject.id * INT64CONST(10000000000);

	idval.type = jbvNumeric;
	idval.val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric,
															Int64GetDatum(id)));

	it = JsonbIteratorInit(jbc);

	while ((tok = JsonbIteratorNext(&it, &key, true)) != WJB_DONE)
	{
		JsonBaseObjectInfo baseObject;
		JsonItem	obj;
		JsonbParseState *ps;
		JsonbValue *keyval;
		Jsonb	   *jsonb;

		if (tok != WJB_KEY)
			continue;

		res = jperOk;

		if (!hasNext && !found)
			break;

		tok = JsonbIteratorNext(&it, &val, true);
		Assert(tok == WJB_VALUE);

		ps = NULL;
		pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);

		pushJsonbValue(&ps, WJB_KEY, &keystr);
		pushJsonbValue(&ps, WJB_VALUE, &key);

		pushJsonbValue(&ps, WJB_KEY, &valstr);
		pushJsonbValue(&ps, WJB_VALUE, &val);

		pushJsonbValue(&ps, WJB_KEY, &idstr);
		pushJsonbValue(&ps, WJB_VALUE, &idval);

		keyval = pushJsonbValue(&ps, WJB_END_OBJECT, NULL);

		jsonb = JsonbValueToJsonb(keyval);

		JsonbInitBinary(&obj.jbv, jsonb);

		baseObject = setBaseObject(cxt, &obj, cxt->lastGeneratedObjectId++);

		res = executeNextItem(cxt, jsp, &next, &obj, found, true);

		cxt->baseObject = baseObject;

		if (jperIsError(res))
			return res;

		if (res == jperOk && !found)
			break;
	}

	return res;
}

/*
 * Convert boolean execution status 'res' to a boolean JSON item and execute
 * next jsonpath.
 */
static JsonPathExecResult
appendBoolResult(JsonPathExecContext *cxt, JsonPathItem *jsp,
				 JsonValueList *found, JsonPathBool res)
{
	JsonPathItem next;
	JsonItem	jsi;

	if (!jspGetNext(jsp, &next) && !found)
		return jperOk;			/* found singleton boolean value */

	if (res == jpbUnknown)
	{
		jsi.jbv.type = jbvNull;
	}
	else
	{
		jsi.jbv.type = jbvBool;
		jsi.jbv.val.boolean = res == jpbTrue;
	}

	return executeNextItem(cxt, jsp, &next, &jsi, found, true);
}

/*
 * Convert jsonpath's scalar or variable node to actual jsonb value.
 *
 * If node is a variable then its id returned, otherwise 0 returned.
 */
static void
getJsonPathItem(JsonPathExecContext *cxt, JsonPathItem *item,
				JsonItem *value)
{
	switch (item->type)
	{
		case jpiNull:
			value->type = jbvNull;
			break;
		case jpiBool:
			value->type = jbvBool;
			value->jbv.val.boolean = jspGetBool(item);
			break;
		case jpiNumeric:
			value->type = jbvNumeric;
			value->jbv.val.numeric = jspGetNumeric(item);
			break;
		case jpiString:
			value->type = jbvString;
			value->jbv.val.string.val = jspGetString(item,
													 &value->jbv.val.string.len);
			break;
		case jpiVariable:
			getJsonPathVariable(cxt, item, value);
			return;
		default:
			elog(ERROR, "unexpected jsonpath item type");
	}
}

/*
 * Get the value of variable passed to jsonpath executor
 */
static void
getJsonPathVariable(JsonPathExecContext *cxt, JsonPathItem *variable,
					JsonItem *value)
{
	char	   *varName;
	int			varNameLength;
	JsonItem	baseObject;
	int			baseObjectId;

	Assert(variable->type == jpiVariable);
	varName = jspGetString(variable, &varNameLength);

	if (!cxt->vars ||
		(baseObjectId = cxt->getVar(cxt->vars, varName, varNameLength, value,
									&baseObject.jbv)) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("cannot find jsonpath variable '%s'",
						pnstrdup(varName, varNameLength))));

	if (baseObjectId > 0)
		setBaseObject(cxt, &baseObject, baseObjectId);
}

static int
getJsonPathVariableFromJsonb(void *varsJsonb, char *varName, int varNameLength,
							 JsonItem *value, JsonbValue *baseObject)
{
	Jsonb	   *vars = varsJsonb;
	JsonbValue	tmp;
	JsonbValue *v;

	if (!varName)
	{
		if (vars && !JsonContainerIsObject(&vars->root))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("jsonb containing jsonpath variables "
							"is not an object")));

		return vars ? 1 : 0;	/* count of base objects */
	}

	tmp.type = jbvString;
	tmp.val.string.val = varName;
	tmp.val.string.len = varNameLength;

	v = findJsonbValueFromContainer(&vars->root, JB_FOBJECT, &tmp);

	if (!v)
		return -1;

	value->jbv = *v;
	pfree(v);

	JsonbInitBinary(baseObject, vars);
	return 1;
}

/**************** Support functions for JsonPath execution *****************/

/*
 * Returns the size of an array item, or -1 if item is not an array.
 */
static int
JsonbArraySize(JsonItem *jb)
{
	Assert(jb->type != jbvArray);

	if (jb->type == jbvBinary)
	{
		JsonbContainer *jbc = jb->jbv.val.binary.data;

		if (JsonContainerIsArray(jbc) && !JsonContainerIsScalar(jbc))
			return JsonContainerSize(jbc);
	}

	return -1;
}

/* Comparison predicate callback. */
static JsonPathBool
executeComparison(JsonPathItem *cmp, JsonItem *lv, JsonItem *rv, void *p)
{
	return compareItems(cmp->type, lv, rv);
}

/*
 * Compare two SQL/JSON items using comparison operation 'op'.
 */
static JsonPathBool
compareItems(int32 op, JsonItem *jsi1, JsonItem *jsi2)
{
	JsonbValue *jb1 = &jsi1->jbv;
	JsonbValue *jb2 = &jsi2->jbv;
	int			cmp;
	bool		res;

	if (jb1->type != jb2->type)
	{
		if (jb1->type == jbvNull || jb2->type == jbvNull)

			/*
			 * Equality and order comparison of nulls to non-nulls returns
			 * always false, but inequality comparison returns true.
			 */
			return op == jpiNotEqual ? jpbTrue : jpbFalse;

		/* Non-null items of different types are not comparable. */
		return jpbUnknown;
	}

	switch (jsi1->type)
	{
		case jbvNull:
			cmp = 0;
			break;
		case jbvBool:
			cmp = jb1->val.boolean == jb2->val.boolean ? 0 :
				jb1->val.boolean ? 1 : -1;
			break;
		case jbvNumeric:
			cmp = compareNumeric(jb1->val.numeric, jb2->val.numeric);
			break;
		case jbvString:
			if (op == jpiEqual)
				return jb1->val.string.len != jb2->val.string.len ||
					memcmp(jb1->val.string.val,
						   jb2->val.string.val,
						   jb1->val.string.len) ? jpbFalse : jpbTrue;

			cmp = varstr_cmp(jb1->val.string.val, jb1->val.string.len,
							 jb2->val.string.val, jb2->val.string.len,
							 DEFAULT_COLLATION_OID);
			break;
		case jsiDatetime:
			{
				bool		error = false;

				cmp = compareDatetime(jsi1->datetime.value,
									  jsi1->datetime.typid,
									  jsi1->datetime.tz,
									  jsi2->datetime.value,
									  jsi2->datetime.typid,
									  jsi2->datetime.tz,
									  &error);

				if (error)
					return jpbUnknown;
			}
			break;

		case jbvBinary:
		case jbvArray:
		case jbvObject:
			return jpbUnknown;	/* non-scalars are not comparable */

		default:
			elog(ERROR, "invalid jsonb value type %d", jb1->type);
	}

	switch (op)
	{
		case jpiEqual:
			res = (cmp == 0);
			break;
		case jpiNotEqual:
			res = (cmp != 0);
			break;
		case jpiLess:
			res = (cmp < 0);
			break;
		case jpiGreater:
			res = (cmp > 0);
			break;
		case jpiLessOrEqual:
			res = (cmp <= 0);
			break;
		case jpiGreaterOrEqual:
			res = (cmp >= 0);
			break;
		default:
			elog(ERROR, "unrecognized jsonpath operation: %d", op);
			return jpbUnknown;
	}

	return res ? jpbTrue : jpbFalse;
}

/* Compare two numerics */
static int
compareNumeric(Numeric a, Numeric b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_cmp,
											 NumericGetDatum(a),
											 NumericGetDatum(b)));
}

static JsonItem *
copyJsonItem(JsonItem *src)
{
	JsonItem *dst = palloc(sizeof(*dst));

	*dst = *src;

	return dst;
}

static JsonbValue *
JsonItemToJsonbValue(JsonItem *jsi, JsonbValue *jbv)
{
	switch (jsi->type)
	{
		case jsiDatetime:
			jbv->type = jbvString;
			jbv->val.string.val = JsonEncodeDateTime(NULL,
													 jsi->datetime.value,
													 jsi->datetime.typid,
													 &jsi->datetime.tz);
			jbv->val.string.len = strlen(jbv->val.string.val);
			return jbv;

		default:
			return &jsi->jbv;
	}
}

static Jsonb *
JsonItemToJsonb(JsonItem *jsi)
{
	JsonbValue	jbv;

	return JsonbValueToJsonb(JsonItemToJsonbValue(jsi, &jbv));
}

static const char *
JsonItemTypeName(JsonItem *jsi)
{
	switch (jsi->type)
	{
		case jsiDatetime:
			switch (jsi->datetime.typid)
			{
				case DATEOID:
					return "date";
				case TIMEOID:
					return "time without time zone";
				case TIMETZOID:
					return "time with time zone";
				case TIMESTAMPOID:
					return "timestamp without time zone";
				case TIMESTAMPTZOID:
					return "timestamp with time zone";
				default:
					elog(ERROR, "unrecognized jsonb value datetime type: %d",
						 jsi->datetime.typid);
					return "unknown";
			}

		default:
			return JsonbTypeName(&jsi->jbv);
	}
}

/*
 * Execute array subscript expression and convert resulting numeric item to
 * the integer type with truncation.
 */
static JsonPathExecResult
getArrayIndex(JsonPathExecContext *cxt, JsonPathItem *jsp, JsonItem *jb,
			  int32 *index)
{
	JsonItem   *jbv;
	JsonValueList found = {0};
	JsonPathExecResult res = executeItem(cxt, jsp, jb, &found);
	Datum		numeric_index;
	bool		have_error = false;

	if (jperIsError(res))
		return res;

	if (JsonValueListLength(&found) != 1 ||
		!(jbv = getScalar(JsonValueListHead(&found), jbvNumeric)))
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_INVALID_JSON_SUBSCRIPT),
							  errmsg(ERRMSG_INVALID_JSON_SUBSCRIPT),
							  errdetail("jsonpath array subscript is not a "
										"singleton numeric value"))));

	numeric_index = DirectFunctionCall2(numeric_trunc,
										NumericGetDatum(jbv->jbv.val.numeric),
										Int32GetDatum(0));

	*index = numeric_int4_opt_error(DatumGetNumeric(numeric_index),
									&have_error);

	if (have_error)
		RETURN_ERROR(ereport(ERROR,
							 (errcode(ERRCODE_INVALID_JSON_SUBSCRIPT),
							  errmsg(ERRMSG_INVALID_JSON_SUBSCRIPT),
							  errdetail("jsonpath array subscript is "
										"out of integer range"))));

	return jperOk;
}

/* Save base object and its id needed for the execution of .keyvalue(). */
static JsonBaseObjectInfo
setBaseObject(JsonPathExecContext *cxt, JsonItem *jbv, int32 id)
{
	JsonBaseObjectInfo baseObject = cxt->baseObject;

	cxt->baseObject.jbc = jbv->type != jbvBinary ? NULL :
		(JsonbContainer *) jbv->jbv.val.binary.data;
	cxt->baseObject.id = id;

	return baseObject;
}

static void
JsonValueListAppend(JsonValueList *jvl, JsonItem *jbv)
{
	if (jvl->singleton)
	{
		jvl->list = list_make2(jvl->singleton, jbv);
		jvl->singleton = NULL;
	}
	else if (!jvl->list)
		jvl->singleton = jbv;
	else
		jvl->list = lappend(jvl->list, jbv);
}

static int
JsonValueListLength(const JsonValueList *jvl)
{
	return jvl->singleton ? 1 : list_length(jvl->list);
}

static bool
JsonValueListIsEmpty(JsonValueList *jvl)
{
	return !jvl->singleton && list_length(jvl->list) <= 0;
}

static JsonItem *
JsonValueListHead(JsonValueList *jvl)
{
	return jvl->singleton ? jvl->singleton : linitial(jvl->list);
}

static List *
JsonValueListGetList(JsonValueList *jvl)
{
	if (jvl->singleton)
		return list_make1(jvl->singleton);

	return jvl->list;
}

static void
JsonValueListInitIterator(const JsonValueList *jvl, JsonValueListIterator *it)
{
	if (jvl->singleton)
	{
		it->value = jvl->singleton;
		it->next = NULL;
	}
	else if (list_head(jvl->list) != NULL)
	{
		it->value = (JsonItem *) linitial(jvl->list);
		it->next = lnext(list_head(jvl->list));
	}
	else
	{
		it->value = NULL;
		it->next = NULL;
	}
}

/*
 * Get the next item from the sequence advancing iterator.
 */
static JsonItem *
JsonValueListNext(const JsonValueList *jvl, JsonValueListIterator *it)
{
	JsonItem *result = it->value;

	if (it->next)
	{
		it->value = lfirst(it->next);
		it->next = lnext(it->next);
	}
	else
	{
		it->value = NULL;
	}

	return result;
}

/*
 * Initialize a binary JsonbValue with the given jsonb container.
 */
static JsonbValue *
JsonbInitBinary(JsonbValue *jbv, Jsonb *jb)
{
	jbv->type = jbvBinary;
	jbv->val.binary.data = &jb->root;
	jbv->val.binary.len = VARSIZE_ANY_EXHDR(jb);

	return jbv;
}

/*
 * Returns jbv* type of of JsonbValue. Note, it never returns jbvBinary as is.
 */
static int
JsonbType(JsonItem *jb)
{
	int			type = jb->type;

	if (jb->type == jbvBinary)
	{
		JsonbContainer *jbc = (void *) jb->jbv.val.binary.data;

		/* Scalars should be always extracted during jsonpath execution. */
		Assert(!JsonContainerIsScalar(jbc));

		if (JsonContainerIsObject(jbc))
			type = jbvObject;
		else if (JsonContainerIsArray(jbc))
			type = jbvArray;
		else
			elog(ERROR, "invalid jsonb container type: 0x%08x", jbc->header);
	}

	return type;
}

/*
 * Convert jsonb to a C-string stripping quotes from scalar strings.
 */
static char *
JsonbValueUnquote(JsonbValue *jbv, int *len)
{
	switch (jbv->type)
	{
		case jbvString:
			*len = jbv->val.string.len;
			return jbv->val.string.val;

		case jbvBool:
			*len = jbv->val.boolean ? 4 : 5;
			return jbv->val.boolean ? "true" : "false";

		case jbvNumeric:
			*len = -1;
			return DatumGetCString(DirectFunctionCall1(numeric_out,
													   PointerGetDatum(jbv->val.numeric)));

		case jbvNull:
			*len = 4;
			return "null";

		case jbvBinary:
			{
				JsonbValue	jbvbuf;

				if (JsonbExtractScalar(jbv->val.binary.data, &jbvbuf))
					return JsonbValueUnquote(&jbvbuf, len);

				*len = -1;
				return JsonbToCString(NULL, jbv->val.binary.data,
									  jbv->val.binary.len);
			}

		default:
			elog(ERROR, "unexpected jsonb value type: %d", jbv->type);
			return NULL;
	}
}

static char *
JsonItemUnquote(JsonItem *jsi, int *len)
{
	switch (jsi->type)
	{
		case jsiDatetime:
			*len = -1;
			return JsonEncodeDateTime(NULL,
									  jsi->datetime.value,
									  jsi->datetime.typid,
									  &jsi->datetime.tz);

		default:
			return JsonbValueUnquote(&jsi->jbv, len);
	}
}

static text *
JsonItemUnquoteText(JsonItem *jsi)
{
	int			len;
	char	   *str = JsonItemUnquote(jsi, &len);

	if (len < 0)
		return cstring_to_text(str);
	else
		return cstring_to_text_with_len(str, len);
}

/* Get scalar of given type or NULL on type mismatch */
static JsonItem *
getScalar(JsonItem *scalar, enum jbvType type)
{
	/* Scalars should be always extracted during jsonpath execution. */
	Assert(scalar->type != jbvBinary ||
		   !JsonContainerIsScalar(scalar->jbv.val.binary.data));

	return scalar->type == type ? scalar : NULL;
}

/* Construct a JSON array from the item list */
static JsonbValue *
wrapItemsInArray(const JsonValueList *items)
{
	JsonbParseState *ps = NULL;
	JsonValueListIterator it;
	JsonItem   *jsi;

	pushJsonbValue(&ps, WJB_BEGIN_ARRAY, NULL);

	JsonValueListInitIterator(items, &it);
	while ((jsi = JsonValueListNext(items, &it)))
	{
		JsonbValue	jbv;

		pushJsonbValue(&ps, WJB_ELEM, JsonItemToJsonbValue(jsi, &jbv));
	}

	return pushJsonbValue(&ps, WJB_END_ARRAY, NULL);
}

static void
pushJsonItem(JsonItemStack *stack, JsonItemStackEntry *entry, JsonItem *item)
{
	entry->item = item;
	entry->parent = *stack;
	*stack = entry;
}

static void
popJsonItem(JsonItemStack *stack)
{
	*stack = (*stack)->parent;
}

static inline Datum
time_to_timetz(Datum time, int tz, bool *error)
{
	TimeADT		tm = DatumGetTimeADT(time);
	TimeTzADT  *result = palloc(sizeof(TimeTzADT));

	if (tz == PG_INT32_MIN)
	{
		*error = true;
		return (Datum) 0;
	}

	result->time = tm;
	result->zone = tz;

	return TimeTzADTPGetDatum(result);
}

static inline Datum
date_to_timestamp(Datum date, bool *error)
{
	DateADT		dt = DatumGetDateADT(date);
	Timestamp	ts = date2timestamp_internal(dt, error);

	return TimestampGetDatum(ts);
}

static inline Datum
date_to_timestamptz(Datum date, int tz, bool *error)
{
	DateADT		dt = DatumGetDateADT(date);
	TimestampTz ts;

	if (tz == PG_INT32_MIN)
	{
		*error = true;
		return (Datum) 0;
	}

	ts = date2timestamptz_internal(dt, &tz, error);

	return TimestampTzGetDatum(ts);
}

static inline Datum
timestamp_to_timestamptz(Datum val, int tz, bool *error)
{
	Timestamp	ts = DatumGetTimestamp(val);
	TimestampTz tstz;

	if (tz == PG_INT32_MIN)
	{
		*error = true;
		return (Datum) 0;
	}

	tstz = timestamp2timestamptz_internal(ts, &tz, error);

	return TimestampTzGetDatum(tstz);
}

/*
 * Cross-type comparison of two datetime SQL/JSON items.  If items are
 * uncomparable, 'error' flag is set.
 */
static int
compareDatetime(Datum val1, Oid typid1, int tz1,
				Datum val2, Oid typid2, int tz2,
				bool *error)
{
	PGFunction cmpfunc = NULL;

	switch (typid1)
	{
		case DATEOID:
			switch (typid2)
			{
				case DATEOID:
					cmpfunc = date_cmp;

					break;

				case TIMESTAMPOID:
					val1 = date_to_timestamp(val1, error);
					cmpfunc = timestamp_cmp;

					break;

				case TIMESTAMPTZOID:
					val1 = date_to_timestamptz(val1, tz1, error);
					cmpfunc = timestamp_cmp;

					break;

				case TIMEOID:
				case TIMETZOID:
					*error = true;
					return 0;
			}
			break;

		case TIMEOID:
			switch (typid2)
			{
				case TIMEOID:
					cmpfunc = time_cmp;

					break;

				case TIMETZOID:
					val1 = time_to_timetz(val1, tz1, error);
					cmpfunc = timetz_cmp;

					break;

				case DATEOID:
				case TIMESTAMPOID:
				case TIMESTAMPTZOID:
					*error = true;
					return 0;
			}
			break;

		case TIMETZOID:
			switch (typid2)
			{
				case TIMEOID:
					val2 = time_to_timetz(val2, tz2, error);
					cmpfunc = timetz_cmp;

					break;

				case TIMETZOID:
					cmpfunc = timetz_cmp;

					break;

				case DATEOID:
				case TIMESTAMPOID:
				case TIMESTAMPTZOID:
					*error = true;
					return 0;
			}
			break;

		case TIMESTAMPOID:
			switch (typid2)
			{
				case DATEOID:
					val2 = date_to_timestamp(val2, error);
					cmpfunc = timestamp_cmp;

					break;

				case TIMESTAMPOID:
					cmpfunc = timestamp_cmp;

					break;

				case TIMESTAMPTZOID:
					val1 = timestamp_to_timestamptz(val1, tz1, error);
					cmpfunc = timestamp_cmp;

					break;

				case TIMEOID:
				case TIMETZOID:
					*error = true;
					return 0;
			}
			break;

		case TIMESTAMPTZOID:
			switch (typid2)
			{
				case DATEOID:
					val2 = date_to_timestamptz(val2, tz2, error);
					cmpfunc = timestamp_cmp;

					break;

				case TIMESTAMPOID:
					val2 = timestamp_to_timestamptz(val2, tz2, error);
					cmpfunc = timestamp_cmp;

					break;

				case TIMESTAMPTZOID:
					cmpfunc = timestamp_cmp;

					break;

				case TIMEOID:
				case TIMETZOID:
					*error = true;
					return 0;
			}
			break;

		default:
			elog(ERROR, "unrecognized SQL/JSON datetime type oid: %d",
				 typid1);
	}

	if (*error)
		return 0;

	if (!cmpfunc)
		elog(ERROR, "unrecognized SQL/JSON datetime type oid: %d",
			 typid2);

	*error = false;

	return DatumGetInt32(DirectFunctionCall2(cmpfunc, val1, val2));
}

/*
 * Try to parse datetime text with the specified datetime template and
 * default time-zone 'tzname'.
 * Returns 'value' datum, its type 'typid' and 'typmod'.
 * Datetime error is rethrown with SQL/JSON errcode if 'throwErrors' is true.
 */
static bool
tryToParseDatetime(text *fmt, text *datetime, char *tzname, bool strict,
				   Datum *value, Oid *typid, int32 *typmod, int *tzp,
				   bool throwErrors)
{
	bool		error = false;
	int			tz = *tzp;

	*value = parse_datetime(datetime, fmt, tzname, strict, typid, typmod,
							&tz, throwErrors ? NULL : &error);

	if (!error)
		*tzp = tz;

	return !error;
}
