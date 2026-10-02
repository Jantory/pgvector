#include "postgres.h"

#include "access/genam.h"
#include "access/tableam.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "qkdtree.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/rel.h"

typedef struct QKDTreeBuildState
{
	Relation	heap;
	Relation	index;
	IndexInfo  *index_info;
	ForkNumber	fork_num;
	int			dimensions;
	int			branch_factor;
	int			leaf_size;
	double		reltuples;
	double		indtuples;
	QKDTreePointTuple *points;
	int64		point_count;
	int64		point_capacity;
} QKDTreeBuildState;

static void
InitRawPage(Buffer buffer, Page page, int page_kind, BlockNumber next_block)
{
	QKDTreePageOpaque opaque;

	PageInit(page, BufferGetPageSize(buffer), sizeof(QKDTreePageOpaqueData));
	opaque = QKDTreePageGetOpaque(page);
	opaque->next_block = next_block;
	opaque->page_kind = page_kind;
	opaque->page_id = QKDTREE_PAGE_ID;
}

static Buffer
NewBuildBuffer(Relation index, ForkNumber fork_num, int page_kind)
{
	Buffer		buffer;
	Page		page;

	LockRelationForExtension(index, ExclusiveLock);
	buffer = ReadBufferExtended(index, fork_num, P_NEW, RBM_NORMAL, NULL);
	UnlockRelationForExtension(index, ExclusiveLock);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	InitRawPage(buffer, page, page_kind, InvalidBlockNumber);
	return buffer;
}

static int
CompareLexicographic(QKDTreePointTuple a, QKDTreePointTuple b, int dimensions)
{
	for (int i = 0; i < dimensions; i++)
	{
		if (a->coords[i] < b->coords[i])
			return -1;
		if (a->coords[i] > b->coords[i])
			return 1;
	}
	return 0;
}

static int
CompareDimension(QKDTreePointTuple a, QKDTreePointTuple b, int dimension)
{
	if (a->coords[dimension] < b->coords[dimension])
		return -1;
	if (a->coords[dimension] > b->coords[dimension])
		return 1;
	return 0;
}

/* Stable merge sort, matching NumPy's stable sort in the prototype. */
static void
StableSortPoints(QKDTreePointTuple *points, int64 count, int dimensions,
				 int split_dimension, bool lexicographic)
{
	QKDTreePointTuple *source;
	QKDTreePointTuple *dest;
	QKDTreePointTuple *tmp;
	int64		width;

	if (count < 2)
		return;

	tmp = palloc(sizeof(QKDTreePointTuple) * count);
	source = points;
	dest = tmp;

	for (width = 1; width < count; width *= 2)
	{
		for (int64 left = 0; left < count; left += 2 * width)
		{
			int64		mid = Min(left + width, count);
			int64		right = Min(left + 2 * width, count);
			int64		i = left;
			int64		j = mid;
			int64		out = left;

			while (i < mid && j < right)
			{
				int			cmp;

				if (lexicographic)
					cmp = CompareLexicographic(source[i], source[j], dimensions);
				else
					cmp = CompareDimension(source[i], source[j], split_dimension);

				/* Choose the left item on equality to preserve stability. */
				if (cmp <= 0)
					dest[out++] = source[i++];
				else
					dest[out++] = source[j++];
			}
			while (i < mid)
				dest[out++] = source[i++];
			while (j < right)
				dest[out++] = source[j++];
		}

		{
			QKDTreePointTuple *swap = source;

			source = dest;
			dest = swap;
		}

		if (width > count / 2)
			break;
	}

	if (source != points)
		memcpy(points, source, sizeof(QKDTreePointTuple) * count);
	pfree(tmp);
}

static BlockNumber
WriteLeafData(QKDTreeBuildState *state, QKDTreePointTuple *points, int64 count)
{
	Buffer		buffer;
	Page		page;
	BlockNumber first_block;
	Size		point_size = QKDTreePointSize(state->dimensions);

	if (count == 0)
		return InvalidBlockNumber;

	buffer = NewBuildBuffer(state->index, state->fork_num, QKDTREE_PAGE_DATA);
	page = BufferGetPage(buffer);
	first_block = BufferGetBlockNumber(buffer);

	for (int64 i = 0; i < count; i++)
	{
		if (PageGetFreeSpace(page) < point_size)
		{
			Buffer		new_buffer = NewBuildBuffer(state->index, state->fork_num,
											QKDTREE_PAGE_DATA);
			BlockNumber new_block = BufferGetBlockNumber(new_buffer);

			QKDTreePageGetOpaque(page)->next_block = new_block;
			MarkBufferDirty(buffer);
			UnlockReleaseBuffer(buffer);

			buffer = new_buffer;
			page = BufferGetPage(buffer);
		}

		if (PageAddItem(page, (Item) points[i], point_size,
						InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add qkdtree point");
	}

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);
	return first_block;
}

static BlockNumber
BuildNode(QKDTreeBuildState *state, QKDTreePointTuple *points, int64 count)
{
	int64	   *sum_vec = palloc0(sizeof(int64) * state->dimensions);
	int16	   *mins = palloc(sizeof(int16) * state->dimensions);
	int16	   *maxs = palloc(sizeof(int16) * state->dimensions);
	int			split_dim = 0;
	int64		max_spread = 0;
	BlockNumber block;
	QKDTreeNode node;

	if (count > 0)
	{
		memcpy(mins, points[0]->coords, sizeof(int16) * state->dimensions);
		memcpy(maxs, points[0]->coords, sizeof(int16) * state->dimensions);
	}
	else
	{
		MemSet(mins, 0, sizeof(int16) * state->dimensions);
		MemSet(maxs, 0, sizeof(int16) * state->dimensions);
	}

	for (int64 i = 0; i < count; i++)
	{
		for (int d = 0; d < state->dimensions; d++)
		{
			int16		value = points[i]->coords[d];

			sum_vec[d] += value;
			if (value < mins[d])
				mins[d] = value;
			if (value > maxs[d])
				maxs[d] = value;
		}
	}

	for (int d = 0; d < state->dimensions; d++)
	{
		int64		spread = (int64) maxs[d] - (int64) mins[d];

		if (spread > max_spread)
		{
			max_spread = spread;
			split_dim = d;
		}
	}

	if (count <= state->leaf_size || max_spread == 0)
	{
		BlockNumber first_data = WriteLeafData(state, points, count);

		node = QKDTreeMakeNode(QKDTREE_NODE_LEAF, state->dimensions, 0,
							   -1, count, first_data, sum_vec, NULL, NULL);
		block = QKDTreeCreatePage(state->index, state->fork_num,
							  QKDTREE_PAGE_NODE, InvalidBlockNumber,
							  node, QKDTreeNodeSize(state->dimensions, 0), false);
		pfree(node);
	}
	else
	{
		int			nchildren = Min((int64) state->branch_factor, count);
		int64	   *boundaries = palloc(sizeof(int64) * (nchildren - 1));
		BlockNumber *children = palloc(sizeof(BlockNumber) * nchildren);
		int64		base = count / nchildren;
		int64		extra = count % nchildren;
		int64		offset = 0;

		StableSortPoints(points, count, state->dimensions, split_dim, false);

		for (int child = 0; child < nchildren; child++)
		{
			int64		group_count = base + (child < extra ? 1 : 0);

			children[child] = BuildNode(state, points + offset, group_count);
			if (child < nchildren - 1)
				boundaries[child] = points[offset + group_count - 1]->coords[split_dim];
			offset += group_count;
		}

		node = QKDTreeMakeNode(QKDTREE_NODE_INTERNAL, state->dimensions,
							   nchildren, split_dim, count,
							   InvalidBlockNumber, sum_vec, boundaries, children);
		block = QKDTreeCreatePage(state->index, state->fork_num,
							  QKDTREE_PAGE_NODE, InvalidBlockNumber,
							  node, QKDTreeNodeSize(state->dimensions, nchildren), false);

		pfree(node);
		pfree(boundaries);
		pfree(children);
	}

	pfree(sum_vec);
	pfree(mins);
	pfree(maxs);
	return block;
}

static void
BuildCallback(Relation index, ItemPointer tid, Datum *values,
			  bool *isnull, bool tuple_is_alive, void *opaque)
{
	QKDTreeBuildState *state = (QKDTreeBuildState *) opaque;
	Vector	   *vector;
	QKDTreePointTuple point;
	Size		point_size;

	if (isnull[0])
		return;

	vector = DatumGetVector(values[0]);
	point_size = QKDTreePointSize(state->dimensions);
	point = palloc0(point_size);
	point->heap_tid = *tid;
	QKDTreeQuantizeVector(vector, state->dimensions, point->coords);

	if (state->point_count == state->point_capacity)
	{
		state->point_capacity = state->point_capacity == 0 ? 1024 : state->point_capacity * 2;
		if (state->points == NULL)
			state->points = palloc(sizeof(QKDTreePointTuple) * state->point_capacity);
		else
			state->points = repalloc(state->points,
									  sizeof(QKDTreePointTuple) * state->point_capacity);
	}

	state->points[state->point_count++] = point;
	state->indtuples++;

	if ((Pointer) vector != DatumGetPointer(values[0]))
		pfree(vector);
}

static void
BuildIndex(Relation heap, Relation index, IndexInfo *index_info,
		   QKDTreeBuildState *state, ForkNumber fork_num)
{
	QKDTreeOptions *options = (QKDTreeOptions *) index->rd_options;
	QKDTreeMetaPageData meta;
	BlockNumber meta_block;
	BlockNumber root_block;

	MemSet(state, 0, sizeof(*state));
	state->heap = heap;
	state->index = index;
	state->index_info = index_info;
	state->fork_num = fork_num;
	state->dimensions = TupleDescAttr(index->rd_att, 0)->atttypmod;
	state->branch_factor = options ? options->branch_factor : QKDTREE_DEFAULT_BRANCH_FACTOR;
	state->leaf_size = options ? options->leaf_size : QKDTREE_DEFAULT_LEAF_SIZE;

	if (state->dimensions < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("column does not have dimensions")));
	if (state->dimensions > QKDTREE_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("column cannot have more than %d dimensions for qkdtree index",
						QKDTREE_MAX_DIM)));

	MemSet(&meta, 0, sizeof(meta));
	meta.magic_number = QKDTREE_MAGIC_NUMBER;
	meta.version = QKDTREE_VERSION;
	meta.dimensions = state->dimensions;
	meta.branch_factor = state->branch_factor;
	meta.leaf_size = state->leaf_size;
	meta.root_block = InvalidBlockNumber;

	meta_block = QKDTreeCreatePage(index, fork_num, QKDTREE_PAGE_META,
									InvalidBlockNumber, &meta, sizeof(meta), false);
	if (meta_block != QKDTREE_METAPAGE_BLKNO)
		elog(ERROR, "qkdtree metapage is not the first index page");

	if (heap != NULL)
		state->reltuples = table_index_build_scan(heap, index, index_info,
											 true, true, BuildCallback,
											 (void *) state, NULL);

	/* The Python prototype lexicographically orders the input first. */
	StableSortPoints(state->points, state->point_count, state->dimensions, -1, true);
	root_block = BuildNode(state, state->points, state->point_count);

	meta.root_block = root_block;
	QKDTreeRewritePage(index, fork_num, QKDTREE_METAPAGE_BLKNO,
					   QKDTREE_PAGE_META, InvalidBlockNumber,
					   &meta, sizeof(meta), false);

	if (RelationNeedsWAL(index) || fork_num == INIT_FORKNUM)
		log_newpage_range(index, fork_num, 0,
						  RelationGetNumberOfBlocksInFork(index, fork_num), true);

	for (int64 i = 0; i < state->point_count; i++)
		pfree(state->points[i]);
	if (state->points != NULL)
		pfree(state->points);
}

IndexBuildResult *
qkdtreebuild(Relation heap, Relation index, IndexInfo *index_info)
{
	QKDTreeBuildState state;
	IndexBuildResult *result;

	BuildIndex(heap, index, index_info, &state, MAIN_FORKNUM);

	result = palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = state.reltuples;
	result->index_tuples = state.indtuples;
	return result;
}

void
qkdtreebuildempty(Relation index)
{
	QKDTreeBuildState state;
	IndexInfo  *index_info = BuildIndexInfo(index);

	BuildIndex(NULL, index, index_info, &state, INIT_FORKNUM);
}
