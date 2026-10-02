#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/relscan.h"
#include "pgstat.h"
#include "qkdtree.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

typedef struct QKDTreeCandidate
{
	int64		distance;
	uint64		sequence;
	ItemPointerData heap_tid;
} QKDTreeCandidate;

typedef struct QKDTreeBeamNode
{
	BlockNumber block;
	long double score;
	uint64		sequence;
} QKDTreeBeamNode;

typedef struct QKDTreeScanOpaqueData
{
	MemoryContext tmp_context;
	int			dimensions;
	int			branch_factor;
	BlockNumber root_block;
	bool		first;
	QKDTreeCandidate *candidates;
	int64		candidate_count;
	int64		candidate_capacity;
	int64		candidate_index;
	uint64		sequence;
} QKDTreeScanOpaqueData;

typedef QKDTreeScanOpaqueData *QKDTreeScanOpaque;

static int
CompareBeamNodes(const void *left, const void *right)
{
	const QKDTreeBeamNode *a = (const QKDTreeBeamNode *) left;
	const QKDTreeBeamNode *b = (const QKDTreeBeamNode *) right;

	if (a->score < b->score)
		return -1;
	if (a->score > b->score)
		return 1;
	if (a->sequence < b->sequence)
		return -1;
	if (a->sequence > b->sequence)
		return 1;
	return 0;
}

static int
CompareCandidates(const void *left, const void *right)
{
	const QKDTreeCandidate *a = (const QKDTreeCandidate *) left;
	const QKDTreeCandidate *b = (const QKDTreeCandidate *) right;

	if (a->distance < b->distance)
		return -1;
	if (a->distance > b->distance)
		return 1;
	if (a->sequence < b->sequence)
		return -1;
	if (a->sequence > b->sequence)
		return 1;
	return 0;
}

static long double
MeanDistanceSquared(const int16 *query, QKDTreeNode node)
{
	long double scaled = 0;
	long double count = node->count;
	int64	   *sum_vec = QKDTreeNodeSum(node);

	if (node->count <= 0)
		return LDBL_MAX;

	for (int d = 0; d < node->dimensions; d++)
	{
		long double diff = ((long double) query[d] * count) - sum_vec[d];

		scaled += diff * diff;
	}

	return scaled / (count * count);
}

static void
AddCandidate(QKDTreeScanOpaque so, int64 distance, ItemPointer heap_tid)
{
	QKDTreeCandidate *candidate;

	if (so->candidate_count == so->candidate_capacity)
	{
		so->candidate_capacity = so->candidate_capacity == 0 ? 128 : so->candidate_capacity * 2;
		if (so->candidates == NULL)
			so->candidates = palloc(sizeof(QKDTreeCandidate) * so->candidate_capacity);
		else
			so->candidates = repalloc(so->candidates,
										 sizeof(QKDTreeCandidate) * so->candidate_capacity);
	}

	candidate = &so->candidates[so->candidate_count++];
	candidate->distance = distance;
	candidate->sequence = so->sequence++;
	candidate->heap_tid = *heap_tid;
}

static void
ScanLeaf(IndexScanDesc scan, QKDTreeNode node, const int16 *query)
{
	QKDTreeScanOpaque so = (QKDTreeScanOpaque) scan->opaque;
	BlockNumber block = node->first_data_block;

	while (BlockNumberIsValid(block))
	{
		Buffer		buffer = ReadBuffer(scan->indexRelation, block);
		Page		page;
		OffsetNumber max_offset;
		BlockNumber next_block;

		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		if (QKDTreePageGetOpaque(page)->page_id != QKDTREE_PAGE_ID ||
			QKDTreePageGetOpaque(page)->page_kind != QKDTREE_PAGE_DATA)
			elog(ERROR, "qkdtree data page is not valid");

		next_block = QKDTreePageGetOpaque(page)->next_block;
		max_offset = PageGetMaxOffsetNumber(page);
		for (OffsetNumber offset = FirstOffsetNumber;
			 offset <= max_offset;
			 offset = OffsetNumberNext(offset))
		{
			ItemId		item_id = PageGetItemId(page, offset);
			QKDTreePointTuple point;
			int64		distance = 0;

			if (!ItemIdIsNormal(item_id))
				continue;
			point = (QKDTreePointTuple) PageGetItem(page, item_id);
			for (int d = 0; d < so->dimensions; d++)
			{
				int64		diff = (int64) point->coords[d] - query[d];

				distance += diff * diff;
			}
			AddCandidate(so, distance, &point->heap_tid);
		}

		UnlockReleaseBuffer(buffer);
		block = next_block;
	}
}

static void
SearchTree(IndexScanDesc scan, const int16 *query)
{
	QKDTreeScanOpaque so = (QKDTreeScanOpaque) scan->opaque;
	int			beam_width = qkdtree_beam_width;
	QKDTreeBeamNode *beam;
	int			beam_count = 1;

	beam = palloc(sizeof(QKDTreeBeamNode) * beam_width);
	beam[0].block = so->root_block;
	beam[0].score = 0;
	beam[0].sequence = 0;

	while (beam_count > 0)
	{
		int64		next_capacity = (int64) beam_count * so->branch_factor;
		QKDTreeBeamNode *next = next_capacity > 0 ?
			palloc(sizeof(QKDTreeBeamNode) * next_capacity) : NULL;
		int64		next_count = 0;

		for (int i = 0; i < beam_count; i++)
		{
			QKDTreeNode node = QKDTreeReadNode(scan->indexRelation, beam[i].block);

			if (node->node_kind == QKDTREE_NODE_LEAF)
				ScanLeaf(scan, node, query);
			else
			{
				BlockNumber *children = QKDTreeNodeChildren(node);

				for (int child = 0; child < node->nchildren; child++)
				{
					QKDTreeNode child_node = QKDTreeReadNode(scan->indexRelation,
														 children[child]);

					next[next_count].block = children[child];
					next[next_count].score = MeanDistanceSquared(query, child_node);
					next[next_count].sequence = so->sequence++;
					next_count++;
					pfree(child_node);
				}
			}

			pfree(node);
		}

		if (next_count == 0)
		{
			if (next != NULL)
				pfree(next);
			break;
		}

		qsort(next, next_count, sizeof(QKDTreeBeamNode), CompareBeamNodes);
		beam_count = Min((int64) beam_width, next_count);
		memcpy(beam, next, sizeof(QKDTreeBeamNode) * beam_count);
		pfree(next);
	}

	pfree(beam);
	qsort(so->candidates, so->candidate_count,
		  sizeof(QKDTreeCandidate), CompareCandidates);
}

IndexScanDesc
qkdtreebeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	QKDTreeScanOpaque so = palloc0(sizeof(QKDTreeScanOpaqueData));
	QKDTreeMetaPageData meta;

	QKDTreeReadMeta(index, &meta);
	so->dimensions = meta.dimensions;
	so->branch_factor = meta.branch_factor;
	so->root_block = meta.root_block;
	so->first = true;
	so->tmp_context = AllocSetContextCreate(CurrentMemoryContext,
										 "qkdtree scan temporary context",
										 ALLOCSET_DEFAULT_SIZES);
	scan->opaque = so;
	return scan;
}

void
qkdtreerescan(IndexScanDesc scan, ScanKey keys, int nkeys,
			  ScanKey orderbys, int norderbys)
{
	QKDTreeScanOpaque so = (QKDTreeScanOpaque) scan->opaque;

	MemoryContextReset(so->tmp_context);
	so->first = true;
	so->candidates = NULL;
	so->candidate_count = 0;
	so->candidate_capacity = 0;
	so->candidate_index = 0;
	so->sequence = 0;

	if (keys != NULL && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
	if (orderbys != NULL && scan->numberOfOrderBys > 0)
		memmove(scan->orderByData, orderbys,
				scan->numberOfOrderBys * sizeof(ScanKeyData));
}

bool
qkdtreegettuple(IndexScanDesc scan, ScanDirection direction)
{
	QKDTreeScanOpaque so = (QKDTreeScanOpaque) scan->opaque;
	MemoryContext old_context;

	Assert(ScanDirectionIsForward(direction));
	old_context = MemoryContextSwitchTo(so->tmp_context);

	if (so->first)
	{
		Vector	   *query_vector;
		int16	   *query;

		pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
		if (scan->instrument)
			scan->instrument->nsearches++;
#endif

		if (scan->orderByData == NULL || scan->numberOfOrderBys != 1)
			elog(ERROR, "cannot scan qkdtree index without one distance order");
		if (scan->orderByData->sk_flags & SK_ISNULL)
			elog(ERROR, "qkdtree query vector cannot be null");
		if (!IsMVCCSnapshot(scan->xs_snapshot))
			elog(ERROR, "non-MVCC snapshots are not supported with qkdtree");

		query_vector = DatumGetVector(scan->orderByData->sk_argument);
		query = palloc(sizeof(int16) * so->dimensions);
		QKDTreeQuantizeVector(query_vector, so->dimensions, query);
		SearchTree(scan, query);
		so->first = false;

		if ((Pointer) query_vector != DatumGetPointer(scan->orderByData->sk_argument))
			pfree(query_vector);
	}

	if (so->candidate_index >= so->candidate_count)
	{
		MemoryContextSwitchTo(old_context);
		return false;
	}

	scan->xs_heaptid = so->candidates[so->candidate_index++].heap_tid;
	scan->xs_recheck = false;
	scan->xs_recheckorderby = false;
	MemoryContextSwitchTo(old_context);
	return true;
}

void
qkdtreeendscan(IndexScanDesc scan)
{
	QKDTreeScanOpaque so = (QKDTreeScanOpaque) scan->opaque;

	MemoryContextDelete(so->tmp_context);
	pfree(so);
	scan->opaque = NULL;
}
