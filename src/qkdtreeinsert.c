#include "postgres.h"

#include "access/generic_xlog.h"
#include "qkdtree.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"

static void
StableSortByDimension(QKDTreePointTuple *points, int64 count, int dimension)
{
	QKDTreePointTuple *tmp;
	QKDTreePointTuple *source;
	QKDTreePointTuple *dest;

	if (count < 2)
		return;

	tmp = palloc(sizeof(QKDTreePointTuple) * count);
	source = points;
	dest = tmp;

	for (int64 width = 1; width < count; width *= 2)
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
				if (source[i]->coords[dimension] <= source[j]->coords[dimension])
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

static QKDTreePointTuple *
ReadLeafPoints(Relation index, QKDTreeNode leaf, int dimensions, int64 *count)
{
	QKDTreePointTuple *points = NULL;
	int64		capacity = 0;
	BlockNumber block = leaf->first_data_block;
	Size		point_size = QKDTreePointSize(dimensions);

	*count = 0;
	while (BlockNumberIsValid(block))
	{
		Buffer		buffer = ReadBuffer(index, block);
		Page		page;
		BlockNumber next;
		OffsetNumber max_offset;

		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		if (QKDTreePageGetOpaque(page)->page_id != QKDTREE_PAGE_ID ||
			QKDTreePageGetOpaque(page)->page_kind != QKDTREE_PAGE_DATA)
			elog(ERROR, "qkdtree data page is not valid");
		next = QKDTreePageGetOpaque(page)->next_block;
		max_offset = PageGetMaxOffsetNumber(page);

		for (OffsetNumber offset = FirstOffsetNumber;
			 offset <= max_offset;
			 offset = OffsetNumberNext(offset))
		{
			ItemId		item_id = PageGetItemId(page, offset);
			QKDTreePointTuple source;

			if (!ItemIdIsNormal(item_id))
				continue;
			if (*count == capacity)
			{
				capacity = capacity == 0 ? 32 : capacity * 2;
				if (points == NULL)
					points = palloc(sizeof(QKDTreePointTuple) * capacity);
				else
					points = repalloc(points, sizeof(QKDTreePointTuple) * capacity);
			}

			source = (QKDTreePointTuple) PageGetItem(page, item_id);
			points[*count] = palloc(point_size);
			memcpy(points[*count], source, point_size);
			(*count)++;
		}

		UnlockReleaseBuffer(buffer);
		block = next;
	}

	return points;
}

/* Runtime splits are uncommon; one tuple per new page keeps WAL updates simple. */
static BlockNumber
WritePointChain(Relation index, QKDTreePointTuple *points, int64 count,
				int dimensions)
{
	BlockNumber first = InvalidBlockNumber;
	Size		point_size = QKDTreePointSize(dimensions);

	for (int64 i = count; i > 0; i--)
		first = QKDTreeCreatePage(index, MAIN_FORKNUM, QKDTREE_PAGE_DATA,
								  first, points[i - 1], point_size, true);
	return first;
}

static void
LinkNewTail(Relation index, BlockNumber tail, BlockNumber new_block)
{
	Buffer		buffer = ReadBuffer(index, tail);
	GenericXLogState *state;
	Page		page;

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buffer, 0);
	if (QKDTreePageGetOpaque(page)->page_id != QKDTREE_PAGE_ID ||
		QKDTreePageGetOpaque(page)->page_kind != QKDTREE_PAGE_DATA)
		elog(ERROR, "qkdtree data page is not valid");
	QKDTreePageGetOpaque(page)->next_block = new_block;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buffer);
}

static BlockNumber
AppendPoint(Relation index, BlockNumber first, QKDTreePointTuple point,
			int dimensions)
{
	BlockNumber new_block = QKDTreeCreatePage(index, MAIN_FORKNUM,
										QKDTREE_PAGE_DATA, InvalidBlockNumber,
										point, QKDTreePointSize(dimensions), true);

	if (!BlockNumberIsValid(first))
		return new_block;

	{
		BlockNumber tail = first;

		for (;;)
		{
			Buffer		buffer = ReadBuffer(index, tail);
			Page		page;
			BlockNumber next;

			LockBuffer(buffer, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buffer);
			next = QKDTreePageGetOpaque(page)->next_block;
			UnlockReleaseBuffer(buffer);
			if (!BlockNumberIsValid(next))
				break;
			tail = next;
		}

		LinkNewTail(index, tail, new_block);
	}

	return first;
}

static BlockNumber
MakeLeaf(Relation index, QKDTreePointTuple *points, int64 count, int dimensions)
{
	int64	   *sum_vec = palloc0(sizeof(int64) * dimensions);
	BlockNumber first_data;
	QKDTreeNode node;
	BlockNumber block;

	for (int64 i = 0; i < count; i++)
		for (int d = 0; d < dimensions; d++)
			sum_vec[d] += points[i]->coords[d];

	first_data = WritePointChain(index, points, count, dimensions);
	node = QKDTreeMakeNode(QKDTREE_NODE_LEAF, dimensions, 0, -1,
						   count, first_data, sum_vec, NULL, NULL);
	block = QKDTreeCreatePage(index, MAIN_FORKNUM, QKDTREE_PAGE_NODE,
							  InvalidBlockNumber, node,
							  QKDTreeNodeSize(dimensions, 0), true);
	pfree(node);
	pfree(sum_vec);
	return block;
}

static void
UpdateAncestor(Relation index, BlockNumber block, int dimensions,
			   const int16 *coords)
{
	QKDTreeNode old_node = QKDTreeReadNode(index, block);
	int64	   *sum_vec = QKDTreeNodeSum(old_node);
	QKDTreeNode new_node;

	for (int d = 0; d < dimensions; d++)
		sum_vec[d] += coords[d];
	new_node = QKDTreeMakeNode(old_node->node_kind, dimensions,
							   old_node->nchildren, old_node->split_dim,
							   old_node->count + 1,
							   old_node->first_data_block, sum_vec,
							   old_node->nchildren > 0 ? QKDTreeNodeBoundaries(old_node) : NULL,
							   old_node->nchildren > 0 ? QKDTreeNodeChildren(old_node) : NULL);
	QKDTreeRewritePage(index, MAIN_FORKNUM, block, QKDTREE_PAGE_NODE,
					   InvalidBlockNumber, new_node,
					   QKDTreeNodeSize(dimensions, old_node->nchildren), true);
	pfree(new_node);
	pfree(old_node);
}

static void
InsertPoint(Relation index, QKDTreePointTuple point)
{
	QKDTreeMetaPageData meta;
	BlockNumber *path;
	int			path_count = 0;
	int			path_capacity = 32;
	BlockNumber block;
	QKDTreeNode leaf;
	QKDTreePointTuple *points;
	int64		point_count;
	int			split_dim = 0;
	int64		max_spread = 0;

	QKDTreeReadMeta(index, &meta);
	path = palloc(sizeof(BlockNumber) * path_capacity);
	block = meta.root_block;

	for (;;)
	{
		QKDTreeNode node = QKDTreeReadNode(index, block);

		if (path_count == path_capacity)
		{
			path_capacity *= 2;
			path = repalloc(path, sizeof(BlockNumber) * path_capacity);
		}
		path[path_count++] = block;

		if (node->node_kind == QKDTREE_NODE_LEAF)
		{
			leaf = node;
			break;
		}
		else
		{
			int			low = 0;
			int			high = node->nchildren - 1;
			int64	   *boundaries = QKDTreeNodeBoundaries(node);
			BlockNumber *children = QKDTreeNodeChildren(node);

			/* NumPy searchsorted(..., side="left"). */
			while (low < high)
			{
				int			mid = low + (high - low) / 2;

				if (boundaries[mid] < point->coords[node->split_dim])
					low = mid + 1;
				else
					high = mid;
			}
			block = children[low];
			pfree(node);
		}
	}

	points = ReadLeafPoints(index, leaf, meta.dimensions, &point_count);
	points = points == NULL ? palloc(sizeof(QKDTreePointTuple)) :
		repalloc(points, sizeof(QKDTreePointTuple) * (point_count + 1));
	points[point_count++] = point;

	for (int d = 0; d < meta.dimensions; d++)
	{
		int16		minimum = points[0]->coords[d];
		int16		maximum = minimum;

		for (int64 i = 1; i < point_count; i++)
		{
			minimum = Min(minimum, points[i]->coords[d]);
			maximum = Max(maximum, points[i]->coords[d]);
		}

		if ((int64) maximum - minimum > max_spread)
		{
			max_spread = (int64) maximum - minimum;
			split_dim = d;
		}
	}

	if (point_count > meta.leaf_size && max_spread > 0)
	{
		int			nchildren = Min((int64) meta.branch_factor, point_count);
		int64	   *boundaries = palloc(sizeof(int64) * (nchildren - 1));
		BlockNumber *children = palloc(sizeof(BlockNumber) * nchildren);
		int64		base = point_count / nchildren;
		int64		extra = point_count % nchildren;
		int64		offset = 0;
		int64	   *sum_vec = palloc0(sizeof(int64) * meta.dimensions);
		QKDTreeNode internal;

		StableSortByDimension(points, point_count, split_dim);
		for (int64 i = 0; i < point_count; i++)
			for (int d = 0; d < meta.dimensions; d++)
				sum_vec[d] += points[i]->coords[d];

		for (int child = 0; child < nchildren; child++)
		{
			int64		group_count = base + (child < extra ? 1 : 0);

			children[child] = MakeLeaf(index, points + offset, group_count,
									   meta.dimensions);
			if (child < nchildren - 1)
				boundaries[child] = points[offset + group_count - 1]->coords[split_dim];
			offset += group_count;
		}

		internal = QKDTreeMakeNode(QKDTREE_NODE_INTERNAL, meta.dimensions,
								   nchildren, split_dim, point_count,
								   InvalidBlockNumber, sum_vec,
								   boundaries, children);
		QKDTreeRewritePage(index, MAIN_FORKNUM, path[path_count - 1],
						   QKDTREE_PAGE_NODE, InvalidBlockNumber,
						   internal,
						   QKDTreeNodeSize(meta.dimensions, nchildren), true);

		pfree(internal);
		pfree(sum_vec);
		pfree(boundaries);
		pfree(children);
	}
	else
	{
		int64	   *sum_vec = QKDTreeNodeSum(leaf);
		BlockNumber first_data;
		QKDTreeNode updated;

		for (int d = 0; d < meta.dimensions; d++)
			sum_vec[d] += point->coords[d];
		first_data = AppendPoint(index, leaf->first_data_block, point,
								meta.dimensions);
		updated = QKDTreeMakeNode(QKDTREE_NODE_LEAF, meta.dimensions, 0,
								 -1, leaf->count + 1, first_data,
								 sum_vec, NULL, NULL);
		QKDTreeRewritePage(index, MAIN_FORKNUM, path[path_count - 1],
						   QKDTREE_PAGE_NODE, InvalidBlockNumber,
						   updated, QKDTreeNodeSize(meta.dimensions, 0), true);
		pfree(updated);
	}

	/* The leaf already contains the new count and sum. */
	for (int i = path_count - 2; i >= 0; i--)
		UpdateAncestor(index, path[i], meta.dimensions, point->coords);

	for (int64 i = 0; i < point_count - 1; i++)
		pfree(points[i]);
	pfree(points);
	pfree(leaf);
	pfree(path);
}

bool
qkdtreeinsert(Relation index, Datum *values, bool *isnull,
			  ItemPointer heap_tid, Relation heap,
			  IndexUniqueCheck check_unique
#if PG_VERSION_NUM >= 140000
			  , bool index_unchanged
#endif
			  , IndexInfo *index_info)
{
	QKDTreeMetaPageData meta;
	QKDTreePointTuple point;
	IntVector *vector;
	MemoryContext insert_context;
	MemoryContext old_context;

	if (isnull[0])
		return false;

	insert_context = AllocSetContextCreate(CurrentMemoryContext,
										"qkdtree insert temporary context",
										ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(insert_context);

	QKDTreeReadMeta(index, &meta);
	vector = DatumGetIntVector(values[0]);
	point = palloc0(QKDTreePointSize(meta.dimensions));
	point->heap_tid = *heap_tid;
	QKDTreeCopyCoordinates(vector, meta.dimensions, point->coords);

	/* Serialize structural changes, just as the Python object does. */
	LockPage(index, QKDTREE_METAPAGE_BLKNO, ExclusiveLock);
	InsertPoint(index, point);
	UnlockPage(index, QKDTREE_METAPAGE_BLKNO, ExclusiveLock);

	if ((Pointer) vector != DatumGetPointer(values[0]))
		pfree(vector);
	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(insert_context);
	return false;
}
