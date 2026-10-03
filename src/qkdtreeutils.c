#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/itup.h"
#include "qkdtree.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"

Size
QKDTreeNodeSize(int dimensions, int nchildren)
{
	Size		size = MAXALIGN(offsetof(QKDTreeNodeData, data));

	size = add_size(size, mul_size(sizeof(int64), dimensions));
	if (nchildren > 0)
	{
		size = add_size(size, mul_size(sizeof(int64), nchildren - 1));
		size = MAXALIGN(size);
		size = add_size(size, mul_size(sizeof(BlockNumber), nchildren));
	}

	return MAXALIGN(size);
}

Size
QKDTreePointSize(int dimensions)
{
	return MAXALIGN(add_size(offsetof(QKDTreePointTupleData, coords),
							 mul_size(sizeof(int16), dimensions)));
}

int64 *
QKDTreeNodeSum(QKDTreeNode node)
{
	return (int64 *) ((char *) node + MAXALIGN(offsetof(QKDTreeNodeData, data)));
}

int64 *
QKDTreeNodeBoundaries(QKDTreeNode node)
{
	return QKDTreeNodeSum(node) + node->dimensions;
}

BlockNumber *
QKDTreeNodeChildren(QKDTreeNode node)
{
	char	   *ptr = (char *) (QKDTreeNodeBoundaries(node) + Max(0, node->nchildren - 1));

	return (BlockNumber *) MAXALIGN(ptr);
}

void
QKDTreeCopyCoordinates(IntVector *vector, int dimensions, int16 *coords)
{
	if (vector->dim != dimensions)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", dimensions, vector->dim)));

	memcpy(coords, vector->x, sizeof(int16) * dimensions);
}

QKDTreeNode
QKDTreeMakeNode(int node_kind, int dimensions, int nchildren,
				int split_dim, int64 count,
				BlockNumber first_data_block,
				const int64 *sum_vec,
				const int64 *boundaries,
				const BlockNumber *children)
{
	Size		size = QKDTreeNodeSize(dimensions, nchildren);
	QKDTreeNode node = palloc0(size);

	node->node_kind = node_kind;
	node->dimensions = dimensions;
	node->nchildren = nchildren;
	node->split_dim = split_dim;
	node->count = count;
	node->first_data_block = first_data_block;

	if (sum_vec != NULL)
		memcpy(QKDTreeNodeSum(node), sum_vec, sizeof(int64) * dimensions);

	if (nchildren > 0)
	{
		memcpy(QKDTreeNodeBoundaries(node), boundaries,
			   sizeof(int64) * (nchildren - 1));
		memcpy(QKDTreeNodeChildren(node), children,
			   sizeof(BlockNumber) * nchildren);
	}

	return node;
}

static void
QKDTreeInitPage(Buffer buffer, Page page, int page_kind, BlockNumber next_block)
{
	QKDTreePageOpaque opaque;

	PageInit(page, BufferGetPageSize(buffer), sizeof(QKDTreePageOpaqueData));
	opaque = QKDTreePageGetOpaque(page);
	opaque->next_block = next_block;
	opaque->page_kind = page_kind;
	opaque->page_id = QKDTREE_PAGE_ID;
}

BlockNumber
QKDTreeCreatePage(Relation index, ForkNumber fork_num,
				  int page_kind, BlockNumber next_block,
				  const void *item, Size item_size, bool use_wal)
{
	Buffer		buffer;
	Page		page;
	BlockNumber block;

	if (item_size > BLCKSZ - MAXALIGN(SizeOfPageHeaderData) -
		MAXALIGN(sizeof(QKDTreePageOpaqueData)) - sizeof(ItemIdData))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("qkdtree item is too large for an index page")));

	LockRelationForExtension(index, ExclusiveLock);
	buffer = ReadBufferExtended(index, fork_num, P_NEW, RBM_NORMAL, NULL);
	UnlockRelationForExtension(index, ExclusiveLock);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	block = BufferGetBlockNumber(buffer);

	if (use_wal)
	{
		GenericXLogState *state = GenericXLogStart(index);

		page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
		QKDTreeInitPage(buffer, page, page_kind, next_block);
		if (item != NULL &&
			PageAddItem(page, (Item) item, item_size,
						InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add qkdtree index item");
		GenericXLogFinish(state);
	}
	else
	{
		page = BufferGetPage(buffer);
		QKDTreeInitPage(buffer, page, page_kind, next_block);
		if (item != NULL &&
			PageAddItem(page, (Item) item, item_size,
						InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add qkdtree index item");
		MarkBufferDirty(buffer);
	}

	UnlockReleaseBuffer(buffer);
	return block;
}

void
QKDTreeRewritePage(Relation index, ForkNumber fork_num, BlockNumber block,
				   int page_kind, BlockNumber next_block,
				   const void *item, Size item_size, bool use_wal)
{
	Buffer		buffer = ReadBufferExtended(index, fork_num, block, RBM_NORMAL, NULL);
	Page		page;

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	if (use_wal)
	{
		GenericXLogState *state = GenericXLogStart(index);

		page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
		QKDTreeInitPage(buffer, page, page_kind, next_block);
		if (item != NULL &&
			PageAddItem(page, (Item) item, item_size,
						InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to rewrite qkdtree index page");
		GenericXLogFinish(state);
	}
	else
	{
		page = BufferGetPage(buffer);
		QKDTreeInitPage(buffer, page, page_kind, next_block);
		if (item != NULL &&
			PageAddItem(page, (Item) item, item_size,
						InvalidOffsetNumber, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to rewrite qkdtree index page");
		MarkBufferDirty(buffer);
	}
	UnlockReleaseBuffer(buffer);
}

QKDTreeNode
QKDTreeReadNode(Relation index, BlockNumber block)
{
	Buffer		buffer = ReadBuffer(index, block);
	Page		page;
	ItemId		item_id;
	QKDTreeNode source;
	QKDTreeNode result;
	Size		size;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	if (QKDTreePageGetOpaque(page)->page_id != QKDTREE_PAGE_ID ||
		QKDTreePageGetOpaque(page)->page_kind != QKDTREE_PAGE_NODE ||
		PageGetMaxOffsetNumber(page) != FirstOffsetNumber)
		elog(ERROR, "qkdtree node page is not valid");

	item_id = PageGetItemId(page, FirstOffsetNumber);
	source = (QKDTreeNode) PageGetItem(page, item_id);
	size = ItemIdGetLength(item_id);
	result = palloc(size);
	memcpy(result, source, size);
	UnlockReleaseBuffer(buffer);

	return result;
}

void
QKDTreeReadMeta(Relation index, QKDTreeMetaPageData *meta)
{
	Buffer		buffer = ReadBuffer(index, QKDTREE_METAPAGE_BLKNO);
	Page		page;
	ItemId		item_id;
	QKDTreeMetaPageData *source;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	if (QKDTreePageGetOpaque(page)->page_id != QKDTREE_PAGE_ID ||
		QKDTreePageGetOpaque(page)->page_kind != QKDTREE_PAGE_META ||
		PageGetMaxOffsetNumber(page) != FirstOffsetNumber)
		elog(ERROR, "qkdtree metapage is not valid");

	item_id = PageGetItemId(page, FirstOffsetNumber);
	source = (QKDTreeMetaPageData *) PageGetItem(page, item_id);
	memcpy(meta, source, sizeof(*meta));
	UnlockReleaseBuffer(buffer);

	if (meta->magic_number != QKDTREE_MAGIC_NUMBER ||
		meta->version != QKDTREE_VERSION)
		elog(ERROR, "qkdtree index is not valid");
}
