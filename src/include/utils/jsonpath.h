/*-------------------------------------------------------------------------
 *
 * jsonpath.h
 *	Definitions for jsonpath datatype
 *
 * Copyright (c) 2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	src/include/utils/jsonpath.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONPATH_H
#define JSONPATH_H

#include "fmgr.h"
#include "executor/tablefunc.h"
#include "utils/jsonb.h"
#include "utils/jsonapi.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "utils/jsonb.h"

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint32		header;			/* version and flags (see below) */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} JsonPath;

#define JSONPATH_VERSION	(0x01)
#define JSONPATH_LAX		(0x80000000)
#define JSONPATH_HDRSZ		(offsetof(JsonPath, data))

#define DatumGetJsonPathP(d)			((JsonPath *) DatumGetPointer(PG_DETOAST_DATUM(d)))
#define DatumGetJsonPathPCopy(d)		((JsonPath *) DatumGetPointer(PG_DETOAST_DATUM_COPY(d)))
#define PG_GETARG_JSONPATH_P(x)			DatumGetJsonPathP(PG_GETARG_DATUM(x))
#define PG_GETARG_JSONPATH_P_COPY(x)	DatumGetJsonPathPCopy(PG_GETARG_DATUM(x))
#define PG_RETURN_JSONPATH_P(p)			PG_RETURN_POINTER(p)

#define jspIsScalar(type) ((type) >= jpiNull && (type) <= jpiBool)

/*
 * All node's type of jsonpath expression
 */
typedef enum JsonPathItemType
{
	jpiNull = jbvNull,			/* NULL literal */
	jpiString = jbvString,		/* string literal */
	jpiNumeric = jbvNumeric,	/* numeric literal */
	jpiBool = jbvBool,			/* boolean literal: TRUE or FALSE */
	jpiAnd,						/* predicate && predicate */
	jpiOr,						/* predicate || predicate */
	jpiNot,						/* ! predicate */
	jpiIsUnknown,				/* (predicate) IS UNKNOWN */
	jpiEqual,					/* expr == expr */
	jpiNotEqual,				/* expr != expr */
	jpiLess,					/* expr < expr */
	jpiGreater,					/* expr > expr */
	jpiLessOrEqual,				/* expr <= expr */
	jpiGreaterOrEqual,			/* expr >= expr */
	jpiAdd,						/* expr + expr */
	jpiSub,						/* expr - expr */
	jpiMul,						/* expr * expr */
	jpiDiv,						/* expr / expr */
	jpiMod,						/* expr % expr */
	jpiPlus,					/* + expr */
	jpiMinus,					/* - expr */
	jpiAnyArray,				/* [*] */
	jpiAnyKey,					/* .* */
	jpiIndexArray,				/* [subscript, ...] */
	jpiAny,						/* .** */
	jpiKey,						/* .key */
	jpiCurrent,					/* @ */
	jpiRoot,					/* $ */
	jpiVariable,				/* $variable */
	jpiFilter,					/* ? (predicate) */
	jpiExists,					/* EXISTS (expr) predicate */
	jpiType,					/* .type() item method */
	jpiSize,					/* .size() item method */
	jpiAbs,						/* .abs() item method */
	jpiFloor,					/* .floor() item method */
	jpiCeiling,					/* .ceiling() item method */
	jpiDouble,					/* .double() item method */
	jpiDatetime,				/* .datetime() item method */
	jpiKeyValue,				/* .keyvalue() item method */
	jpiSubscript,				/* array subscript: 'expr' or 'expr TO expr' */
	jpiLast,					/* LAST array subscript */
	jpiStartsWith,				/* STARTS WITH predicate */
	jpiLikeRegex,				/* LIKE_REGEX predicate */
} JsonPathItemType;

/* XQuery regex mode flags for LIKE_REGEX predicate */
#define JSP_REGEX_ICASE		0x01	/* i flag, case insensitive */
#define JSP_REGEX_SLINE		0x02	/* s flag, single-line mode */
#define JSP_REGEX_MLINE		0x04	/* m flag, multi-line mode */
#define JSP_REGEX_WSPACE	0x08	/* x flag, expanded syntax */
#define JSP_REGEX_QUOTE		0x10	/* q flag, no special characters */

/*
 * Support functions to parse/construct binary value.
 * Unlike many other representation of expression the first/main
 * node is not an operation but left operand of expression. That
 * allows to implement cheap follow-path descending in jsonb
 * structure and then execute operator with right operand
 */

typedef struct JsonPathItem
{
	JsonPathItemType type;

	/* position form base to next node */
	int32		nextPos;

	/*
	 * pointer into JsonPath value to current node, all positions of current
	 * are relative to this base
	 */
	char	   *base;

	union
	{
		/* classic operator with two operands: and, or etc */
		struct
		{
			int32		left;
			int32		right;
		}			args;

		/* any unary operation */
		int32		arg;

		/* storage for jpiIndexArray: indexes of array */
		struct
		{
			int32		nelems;
			struct
			{
				int32		from;
				int32		to;
			}		   *elems;
		}			array;

		/* jpiAny: levels */
		struct
		{
			uint32		first;
			uint32		last;
		}			anybounds;

		struct
		{
			char	   *data;	/* for bool, numeric and string/key */
			int32		datalen;	/* filled only for string/key */
		}			value;

		struct
		{
			int32		expr;
			char	   *pattern;
			int32		patternlen;
			uint32		flags;
		}			like_regex;
	}			content;
} JsonPathItem;

#define jspHasNext(jsp) ((jsp)->nextPos > 0)

extern void jspInit(JsonPathItem *v, JsonPath *js);
extern void jspInitByBuffer(JsonPathItem *v, char *base, int32 pos);
extern bool jspGetNext(JsonPathItem *v, JsonPathItem *a);
extern void jspGetArg(JsonPathItem *v, JsonPathItem *a);
extern void jspGetLeftArg(JsonPathItem *v, JsonPathItem *a);
extern void jspGetRightArg(JsonPathItem *v, JsonPathItem *a);
extern Numeric jspGetNumeric(JsonPathItem *v);
extern bool jspGetBool(JsonPathItem *v);
extern char *jspGetString(JsonPathItem *v, int32 *len);
extern bool jspGetArraySubscript(JsonPathItem *v, JsonPathItem *from,
								 JsonPathItem *to, int i);

extern const char *jspOperationName(JsonPathItemType type);

/*
 * Parsing support data structures.
 */

typedef struct JsonPathParseItem JsonPathParseItem;

struct JsonPathParseItem
{
	JsonPathItemType type;
	JsonPathParseItem *next;	/* next in path */

	union
	{

		/* classic operator with two operands: and, or etc */
		struct
		{
			JsonPathParseItem *left;
			JsonPathParseItem *right;
		}			args;

		/* any unary operation */
		JsonPathParseItem *arg;

		/* storage for jpiIndexArray: indexes of array */
		struct
		{
			int			nelems;
			struct
			{
				JsonPathParseItem *from;
				JsonPathParseItem *to;
			}		   *elems;
		}			array;

		/* jpiAny: levels */
		struct
		{
			uint32		first;
			uint32		last;
		}			anybounds;

		struct
		{
			JsonPathParseItem *expr;
			char	   *pattern;	/* could not be not null-terminated */
			uint32		patternlen;
			uint32		flags;
		}			like_regex;

		/* scalars */
		Numeric numeric;
		bool		boolean;
		struct
		{
			uint32		len;
			char	   *val;	/* could not be not null-terminated */
		}			string;
	}			value;
};

typedef struct JsonPathParseResult
{
	JsonPathParseItem *expr;
	bool		lax;
} JsonPathParseResult;

extern JsonPathParseResult *parsejsonpath(const char *str, int len);

/*
 * Evaluation of jsonpath
 */

/* External variable passed into jsonpath. */
typedef struct JsonPathVariableEvalContext
{
	char	   *name;
	Oid			typid;
	int32		typmod;
	struct ExprContext *econtext;
	struct ExprState  *estate;
	MemoryContext mcxt;		/* memory context for cached value */
	Datum		value;
	bool		isnull;
	bool		evaluated;
} JsonPathVariableEvalContext;

/* Type of SQL/JSON item */
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
typedef struct JsonItem
{
	struct JsonItem *next;

	union
	{
		int			type;	/* XXX JsonItemType */

		JsonbValue	jbv;

		struct
		{
			int			type;
			Datum		value;
			Oid			typid;
			int32		typmod;
			int			tz;
		}			datetime;
	} val;
} JsonItem;

#define JsonItemJbv(jsi)			(&(jsi)->val.jbv)
#define JsonItemBool(jsi)			(JsonItemJbv(jsi)->val.boolean)
#define JsonItemNumeric(jsi)		(JsonItemJbv(jsi)->val.numeric)
#define JsonItemNumericDatum(jsi)	NumericGetDatum(JsonItemNumeric(jsi))
#define JsonItemString(jsi)			(JsonItemJbv(jsi)->val.string)
#define JsonItemBinary(jsi)			(JsonItemJbv(jsi)->val.binary)
#define JsonItemArray(jsi)			(JsonItemJbv(jsi)->val.array)
#define JsonItemObject(jsi)			(JsonItemJbv(jsi)->val.object)
#define JsonItemDatetime(jsi)		((jsi)->val.datetime)

#define JsonItemGetType(jsi)		((jsi)->val.type)
#define JsonItemIsNull(jsi)			(JsonItemGetType(jsi) == jsiNull)
#define JsonItemIsBool(jsi)			(JsonItemGetType(jsi) == jsiBool)
#define JsonItemIsNumeric(jsi)		(JsonItemGetType(jsi) == jsiNumeric)
#define JsonItemIsString(jsi)		(JsonItemGetType(jsi) == jsiString)
#define JsonItemIsBinary(jsi)		(JsonItemGetType(jsi) == jsiBinary)
#define JsonItemIsArray(jsi)		(JsonItemGetType(jsi) == jsiArray)
#define JsonItemIsObject(jsi)		(JsonItemGetType(jsi) == jsiObject)
#define JsonItemIsDatetime(jsi)		(JsonItemGetType(jsi) == jsiDatetime)
#define JsonItemIsScalar(jsi)		(IsAJsonbScalar(JsonItemJbv(jsi)) || \
									 JsonItemIsDatetime(jsi))

extern Jsonb *JsonItemToJsonb(JsonItem *jsi);
extern Json *JsonItemToJson(JsonItem *jsi);
extern void JsonItemFromDatum(Datum val, Oid typid, int32 typmod,
				  JsonItem *res, bool isJsonb);
extern Datum JsonItemToJsonxDatum(JsonItem *jsi, bool isJsonb);
extern Datum JsonbValueToJsonxDatum(JsonbValue *jbv, bool isJsonb);

extern bool JsonPathExists(Datum jb, JsonPath *path,
			   List *vars, bool isJsonb, bool *error);
extern Datum JsonPathQuery(Datum jb, JsonPath *jp, JsonWrapper wrapper,
			   bool *empty, bool *error, List *vars, bool isJsonb);
extern JsonItem *JsonPathValue(Datum jb, JsonPath *jp, bool *empty,
			   bool *error, List *vars, bool isJsonb);

extern int EvalJsonPathVar(void *vars, bool isJsonb, char *varName,
				int varNameLen, JsonItem *val, JsonbValue *baseObject);

extern const TableFuncRoutine JsonbTableRoutine;

#endif
