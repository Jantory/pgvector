#include "postgres.h"

#include <errno.h>
#include <limits.h>
#include <math.h>

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "intvec.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "vector.h"

#if PG_VERSION_NUM >= 170000
#include "parser/scansup.h"
#endif

static inline void
CheckDims(IntVector *a, IntVector *b)
{
	if (a->dim != b->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("different intvec dimensions %d and %d", a->dim, b->dim)));
}

static inline void
CheckExpectedDim(int32 typmod, int dim)
{
	if (typmod != -1 && typmod != dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", typmod, dim)));
}

static inline void
CheckDim(int dim)
{
	if (dim < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("intvec must have at least 1 dimension")));

	if (dim > INTVEC_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("intvec cannot have more than %d dimensions", INTVEC_MAX_DIM)));
}

IntVector *
InitIntVector(int dim)
{
	Size		size = INTVEC_SIZE(dim);
	IntVector  *result = (IntVector *) palloc0(size);

	SET_VARSIZE(result, size);
	result->dim = dim;
	return result;
}

#if PG_VERSION_NUM >= 170000
#define intvec_isspace(ch) scanner_isspace(ch)
#else
static inline bool
intvec_isspace(char ch)
{
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}
#endif

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(intvec_in);
Datum
intvec_in(PG_FUNCTION_ARGS)
{
	char	   *lit = PG_GETARG_CSTRING(0);
	int32		typmod = PG_GETARG_INT32(2);
	int16		x[INTVEC_MAX_DIM];
	int			dim = 0;
	char	   *pt = lit;
	IntVector  *result;

	while (intvec_isspace(*pt))
		pt++;

	if (*pt != '[')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type intvec: \"%s\"", lit),
				 errdetail("Vector contents must start with \"[\".")));
	pt++;

	while (intvec_isspace(*pt))
		pt++;
	if (*pt == ']')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("intvec must have at least 1 dimension")));

	for (;;)
	{
		long		val;
		char	   *stringEnd;

		if (dim == INTVEC_MAX_DIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("intvec cannot have more than %d dimensions", INTVEC_MAX_DIM)));

		while (intvec_isspace(*pt))
			pt++;
		if (*pt == '\0')
			goto invalid;

		errno = 0;
		val = strtol(pt, &stringEnd, 10);
		if (stringEnd == pt)
			goto invalid;
		if (errno == ERANGE || val < PG_INT16_MIN || val > PG_INT16_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("\"%s\" is out of range for type intvec", pnstrdup(pt, stringEnd - pt))));

		x[dim++] = (int16) val;
		pt = stringEnd;
		while (intvec_isspace(*pt))
			pt++;

		if (*pt == ',')
			pt++;
		else if (*pt == ']')
		{
			pt++;
			break;
		}
		else
			goto invalid;
	}

	while (intvec_isspace(*pt))
		pt++;
	if (*pt != '\0')
		goto invalid;

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);
	result = InitIntVector(dim);
	memcpy(result->x, x, sizeof(int16) * dim);
	PG_RETURN_POINTER(result);

invalid:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type intvec: \"%s\"", lit)));
	PG_RETURN_NULL();
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(intvec_out);
Datum
intvec_out(PG_FUNCTION_ARGS)
{
	IntVector  *vec = PG_GETARG_INTVEC_P(0);
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	for (int i = 0; i < vec->dim; i++)
	{
		if (i > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfo(&buf, "%d", vec->x[i]);
	}
	appendStringInfoChar(&buf, ']');
	PG_FREE_IF_COPY(vec, 0);
	PG_RETURN_CSTRING(buf.data);
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(intvec_typmod_in);
Datum
intvec_typmod_in(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));
	if (*tl < 1 || *tl > INTVEC_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions for type intvec must be between 1 and %d", INTVEC_MAX_DIM)));
	PG_RETURN_INT32(*tl);
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(intvec_recv);
Datum
intvec_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int32		typmod = PG_GETARG_INT32(2);
	int16		dim = pq_getmsgint(buf, sizeof(int16));
	int16		unused = pq_getmsgint(buf, sizeof(int16));
	IntVector  *result;

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);
	if (unused != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected unused to be 0, not %d", unused)));

	result = InitIntVector(dim);
	for (int i = 0; i < dim; i++)
		result->x[i] = (int16) pq_getmsgint(buf, sizeof(int16));
	PG_RETURN_POINTER(result);
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(intvec_send);
Datum
intvec_send(PG_FUNCTION_ARGS)
{
	IntVector  *vec = PG_GETARG_INTVEC_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint(&buf, vec->dim, sizeof(int16));
	pq_sendint(&buf, vec->unused, sizeof(int16));
	for (int i = 0; i < vec->dim; i++)
		pq_sendint(&buf, vec->x[i], sizeof(int16));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(intvec);
Datum
intvec(PG_FUNCTION_ARGS)
{
	IntVector  *vec = PG_GETARG_INTVEC_P(0);
	int32		typmod = PG_GETARG_INT32(1);

	CheckExpectedDim(typmod, vec->dim);
	PG_RETURN_POINTER(vec);
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(vector_to_intvec);
Datum
vector_to_intvec(PG_FUNCTION_ARGS)
{
	Vector	   *vec = PG_GETARG_VECTOR_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	IntVector  *result;

	CheckDim(vec->dim);
	CheckExpectedDim(typmod, vec->dim);
	result = InitIntVector(vec->dim);
	for (int i = 0; i < vec->dim; i++)
	{
		float		value = vec->x[i];

		if (value < PG_INT16_MIN || value > PG_INT16_MAX || truncf(value) != value)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("vector element %g cannot be represented as intvec", value)));
		result->x[i] = (int16) value;
	}
	PG_RETURN_POINTER(result);
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(array_to_intvec);
Datum
array_to_intvec(PG_FUNCTION_ARGS)
{
	ArrayType  *array = PG_GETARG_ARRAYTYPE_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	Datum	   *elements;
	bool	   *nulls;
	int			count;
	IntVector  *result;

	if (ARR_NDIM(array) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("array must be 1-D")));

	deconstruct_array(array, INT4OID, sizeof(int32), true, TYPALIGN_INT,
					  &elements, &nulls, &count);
	CheckDim(count);
	CheckExpectedDim(typmod, count);
	result = InitIntVector(count);

	for (int i = 0; i < count; i++)
	{
		int32		value;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("array must not contain nulls")));
		value = DatumGetInt32(elements[i]);
		if (value < PG_INT16_MIN || value > PG_INT16_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("array element %d cannot be represented as intvec", value)));
		result->x[i] = (int16) value;
	}

	pfree(elements);
	pfree(nulls);
	PG_RETURN_POINTER(result);
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(intvec_to_vector);
Datum
intvec_to_vector(PG_FUNCTION_ARGS)
{
	IntVector  *vec = PG_GETARG_INTVEC_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	Vector	   *result;

	CheckExpectedDim(typmod, vec->dim);
	result = InitVector(vec->dim);
	for (int i = 0; i < vec->dim; i++)
		result->x[i] = (float) vec->x[i];
	PG_RETURN_POINTER(result);
}

static int64
IntvecL2SquaredDistance(IntVector *a, IntVector *b)
{
	int64		distance = 0;

	CheckDims(a, b);
	for (int i = 0; i < a->dim; i++)
	{
		int64		diff = (int64) a->x[i] - (int64) b->x[i];

		distance += diff * diff;
	}
	return distance;
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(intvec_l2_squared_distance);
Datum
intvec_l2_squared_distance(PG_FUNCTION_ARGS)
{
	IntVector  *a = PG_GETARG_INTVEC_P(0);
	IntVector  *b = PG_GETARG_INTVEC_P(1);

	PG_RETURN_FLOAT8((double) IntvecL2SquaredDistance(a, b));
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(intvec_l2_distance);
Datum
intvec_l2_distance(PG_FUNCTION_ARGS)
{
	IntVector  *a = PG_GETARG_INTVEC_P(0);
	IntVector  *b = PG_GETARG_INTVEC_P(1);

	PG_RETURN_FLOAT8(sqrt((double) IntvecL2SquaredDistance(a, b)));
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(intvec_vector_dims);
Datum
intvec_vector_dims(PG_FUNCTION_ARGS)
{
	IntVector  *vec = PG_GETARG_INTVEC_P(0);

	PG_RETURN_INT32(vec->dim);
}
