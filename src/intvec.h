#ifndef INTVEC_H
#define INTVEC_H

#include "fmgr.h"
#include "utils/palloc.h"

#if PG_VERSION_NUM < 190000
#include "storage/shmem.h"
#endif

#define INTVEC_MAX_DIM 16000

#define INTVEC_SIZE(_dim) add_size(offsetof(IntVector, x), mul_size(sizeof(int16), _dim))
#define DatumGetIntVector(x) ((IntVector *) PG_DETOAST_DATUM(x))
#define PG_GETARG_INTVEC_P(x) DatumGetIntVector(PG_GETARG_DATUM(x))
#define PG_RETURN_INTVEC_P(x) PG_RETURN_POINTER(x)

typedef struct IntVector
{
	int32		vl_len_;
	int16		dim;
	int16		unused;
	int16		x[FLEXIBLE_ARRAY_MEMBER];
} IntVector;

IntVector *InitIntVector(int dim);
Datum		intvec_vector_dims(PG_FUNCTION_ARGS);

#endif
