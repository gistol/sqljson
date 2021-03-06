/*-------------------------------------------------------------------------
 *
 * jsonapi.h
 *	  Declarations for JSON API support.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/jsonapi.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONAPI_H
#define JSONAPI_H

#include "jsonb.h"
#include "access/htup.h"
#include "lib/stringinfo.h"

typedef enum
{
	JSON_TOKEN_INVALID,
	JSON_TOKEN_STRING,
	JSON_TOKEN_NUMBER,
	JSON_TOKEN_OBJECT_START,
	JSON_TOKEN_OBJECT_END,
	JSON_TOKEN_ARRAY_START,
	JSON_TOKEN_ARRAY_END,
	JSON_TOKEN_COMMA,
	JSON_TOKEN_COLON,
	JSON_TOKEN_TRUE,
	JSON_TOKEN_FALSE,
	JSON_TOKEN_NULL,
	JSON_TOKEN_END
} JsonTokenType;


/*
 * All the fields in this structure should be treated as read-only.
 *
 * If strval is not null, then it should contain the de-escaped value
 * of the lexeme if it's a string. Otherwise most of these field names
 * should be self-explanatory.
 *
 * line_number and line_start are principally for use by the parser's
 * error reporting routines.
 * token_terminator and prev_token_terminator point to the character
 * AFTER the end of the token, i.e. where there would be a nul byte
 * if we were using nul-terminated strings.
 */
typedef struct JsonLexContext
{
	char	   *input;
	int			input_length;
	char	   *token_start;
	char	   *token_terminator;
	char	   *prev_token_terminator;
	JsonTokenType token_type;
	int			lex_level;
	int			line_number;
	char	   *line_start;
	StringInfo	strval;
	bool		throw_errors;
	bool		error;
} JsonLexContext;

typedef void (*json_struct_action) (void *state);
typedef void (*json_ofield_action) (void *state, char *fname, bool isnull);
typedef void (*json_aelem_action) (void *state, bool isnull);
typedef void (*json_scalar_action) (void *state, char *token, JsonTokenType tokentype);


/*
 * Semantic Action structure for use in parsing json.
 * Any of these actions can be NULL, in which case nothing is done at that
 * point, Likewise, semstate can be NULL. Using an all-NULL structure amounts
 * to doing a pure parse with no side-effects, and is therefore exactly
 * what the json input routines do.
 *
 * The 'fname' and 'token' strings passed to these actions are palloc'd.
 * They are not free'd or used further by the parser, so the action function
 * is free to do what it wishes with them.
 */
typedef struct JsonSemAction
{
	void	   *semstate;
	json_struct_action object_start;
	json_struct_action object_end;
	json_struct_action array_start;
	json_struct_action array_end;
	json_ofield_action object_field_start;
	json_ofield_action object_field_end;
	json_aelem_action array_element_start;
	json_aelem_action array_element_end;
	json_scalar_action scalar;
} JsonSemAction;

typedef enum
{
	JTI_ARRAY_START,
	JTI_ARRAY_ELEM,
	JTI_ARRAY_ELEM_SCALAR,
	JTI_ARRAY_ELEM_AFTER,
	JTI_ARRAY_END,
	JTI_OBJECT_START,
	JTI_OBJECT_KEY,
	JTI_OBJECT_VALUE,
	JTI_OBJECT_VALUE_AFTER,
} JsontIterState;

typedef struct JsonContainerData
{
	uint32		header;
	int			len;
	char	   *data;
} JsonContainerData;

typedef const JsonContainerData JsonContainer;

#define JsonTextContainerSize(jc) \
	(((jc)->header & JB_CMASK) == JB_CMASK && JsonContainerIsArray(jc) \
	 ? JsonGetArraySize(jc) : (jc)->header & JB_CMASK)

typedef struct Json
{
	JsonContainer root;
} Json;

typedef struct JsonIterator
{
	struct JsonIterator	*parent;
	JsonContainer *container;
	JsonLexContext *lex;
	JsontIterState state;
	bool		isScalar;
} JsonIterator;

#define DatumGetJsonP(datum) JsonCreate(DatumGetTextP(datum))
#define DatumGetJsonPCopy(datum) JsonCreate(DatumGetTextPCopy(datum))

#define JsonPGetDatum(json) \
	PointerGetDatum(cstring_to_text_with_len((json)->root.data, (json)->root.len))

#define PG_GETARG_JSON_P(n)			DatumGetJsonP(PG_GETARG_DATUM(n))
#define PG_GETARG_JSON_P_COPY(n)	DatumGetJsonPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_JSON_P(json)		PG_RETURN_DATUM(JsonPGetDatum(json))

/*
 * parse_json will parse the string in the lex calling the
 * action functions in sem at the appropriate points. It is
 * up to them to keep what state they need	in semstate. If they
 * need access to the state of the lexer, then its pointer
 * should be passed to them as a member of whatever semstate
 * points to. If the action pointers are NULL the parser
 * does nothing and just continues.
 */
extern bool pg_parse_json(JsonLexContext *lex, JsonSemAction *sem);

/*
 * json_count_array_elements performs a fast secondary parse to determine the
 * number of elements in passed array lex context. It should be called from an
 * array_start action.
 */
extern int	json_count_array_elements(JsonLexContext *lex);

/*
 * constructors for JsonLexContext, with or without strval element.
 * If supplied, the strval element will contain a de-escaped version of
 * the lexeme. However, doing this imposes a performance penalty, so
 * it should be avoided if the de-escaped lexeme is not required.
 *
 * If you already have the json as a text* value, use the first of these
 * functions, otherwise use  makeJsonLexContextCstringLen().
 */
extern JsonLexContext *makeJsonLexContext(text *json, bool need_escapes);
extern JsonLexContext *makeJsonLexContextCstringLen(char *json,
							 int len,
							 bool need_escapes);

/*
 * Utility function to check if a string is a valid JSON number.
 *
 * str argument does not need to be nul-terminated.
 */
extern bool IsValidJsonNumber(const char *str, int len);

/*
 * Flag types for iterate_json(b)_values to specify what elements from a
 * json(b) document we want to iterate.
 */
typedef enum JsonToIndex
{
	jtiKey = 0x01,
	jtiString = 0x02,
	jtiNumeric = 0x04,
	jtiBool = 0x08,
	jtiAll = jtiKey | jtiString | jtiNumeric | jtiBool
} JsonToIndex;

/* an action that will be applied to each value in iterate_json(b)_vaues functions */
typedef void (*JsonIterateStringValuesAction) (void *state, char *elem_value, int elem_len);

/* an action that will be applied to each value in transform_json(b)_values functions */
typedef text *(*JsonTransformStringValuesAction) (void *state, char *elem_value, int elem_len);

extern uint32 parse_jsonb_index_flags(Jsonb *jb);
extern void iterate_jsonb_values(Jsonb *jb, uint32 flags, void *state,
					 JsonIterateStringValuesAction action);
extern void iterate_json_values(text *json, uint32 flags, void *action_state,
					JsonIterateStringValuesAction action);
extern Jsonb *transform_jsonb_string_values(Jsonb *jsonb, void *action_state,
							  JsonTransformStringValuesAction transform_action);
extern text *transform_json_string_values(text *json, void *action_state,
							 JsonTransformStringValuesAction transform_action);

extern char *JsonEncodeDateTime(char *buf, Datum value, Oid typid,
				   const int *tzp);

extern Datum json_populate_type(Datum json_val, Oid json_type,
								Oid typid, int32 typmod,
								void **cache, MemoryContext mcxt, bool *isnull);

extern Json *JsonCreate(text *json);
extern JsonbIteratorToken JsonIteratorNext(JsonIterator **pit, JsonbValue *val,
				 bool skipNested);
extern JsonIterator *JsonIteratorInit(JsonContainer *jc);
extern void JsonIteratorFree(JsonIterator *it);
extern uint32 JsonGetArraySize(JsonContainer *jc);
extern Json *JsonbValueToJson(JsonbValue *jbv);
extern bool JsonExtractScalar(JsonContainer *jbc, JsonbValue *res);
extern char *JsonUnquote(Json *jb);
extern char *JsonToCString(StringInfo out, JsonContainer *jc,
			  int estimated_len);
extern JsonbValue *pushJsonValue(JsonbParseState **pstate,
			  JsonbIteratorToken tok, JsonbValue *jbv);
extern JsonbValue *findJsonValueFromContainer(JsonContainer *jc, uint32 flags,
						   JsonbValue *key);
extern JsonbValue *getIthJsonValueFromContainer(JsonContainer *array,
							 uint32 index);

#endif							/* JSONAPI_H */
