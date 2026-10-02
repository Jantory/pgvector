#include "postgres.h"

#include "access/generic_xlog.h"
#include "commands/vacuum.h"
#include "qkdtree.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"

#if PG_VERSION_NUM >= 180000
#define vacuum_delay_point() vacuum_delay_point(false)
#endif

typedef struct QKDTreeVacuumResult
{
	int64		count;
	int64	   *sum_vec;
} QKDTreeVacuumResult;

static QKDTreeVacuumResult
VacuumNode(Relation index, BlockNumber block, int dimensions,
		   IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
		   void *callback_state)
{
	QKDTreeNode node = QKDTreeReadNode(index, block);
	QKDTreeVacuumResult result;

	result.count = 0;
	result.sum_vec = palloc0(sizeof(int64) * dimensions);

	if (node->node_kind == QKDTREE_NODE_LEAF)
	{
		BlockNumber data_block = node->first_data_block;

		while (BlockNumberIsValid(data_block))
		{
			Buffer		buffer;
			GenericXLogState *state;
			Page		page;
			BlockNumber next;
			OffsetNumber max_offset;
			OffsetNumber *deletable;
			int			delete_count = 0;

			vacuum_delay_point();
			buffer = ReadBuffer(index, data_block);
			LockBufferForCleanup(buffer);
			state = GenericXLogStart(index);
			page = GenericXLogRegisterBuffer(state, buffer, 0);
			next = QKDTreePageGetOpaque(page)->next_block;
			max_offset = PageGetMaxOffsetNumber(page);
			deletable = palloc(sizeof(OffsetNumber) * Max(1, max_offset));

			for (OffsetNumber offset = FirstOffsetNumber;
				 offset <= max_offset;
				 offset = OffsetNumberNext(offset))
			{
				ItemId		item_id = PageGetItemId(page, offset);
				QKDTreePointTuple point;

				if (!ItemIdIsNormal(item_id))
					continue;
				point = (QKDTreePointTuple) PageGetItem(page, item_id);
				if (callback(&point->heap_tid, callback_state))
				{
					deletable[delete_count++] = offset;
					stats->tuples_removed++;
				}
				else
				{
					result.count++;
					stats->num_index_tuples++;
					for (int d = 0; d < dimensions; d++)
						result.sum_vec[d] += point->coords[d];
				}
			}

			if (delete_count > 0)
			{
				PageIndexMultiDelete(page, deletable, delete_count);
				GenericXLogFinish(state);
			}
			else
				GenericXLogAbort(state);

			pfree(deletable);
			UnlockReleaseBuffer(buffer);
			data_block = next;
		}
	}
	else
	{
		BlockNumber *children = QKDTreeNodeChildren(node);

		for (int child = 0; child < node->nchildren; child++)
		{
			QKDTreeVacuumResult child_result = VacuumNode(index, children[child],
												 dimensions, stats, callback,
												 callback_state);

			result.count += child_result.count;
			for (int d = 0; d < dimensions; d++)
				result.sum_vec[d] += child_result.sum_vec[d];
			pfree(child_result.sum_vec);
		}
	}

	{
		QKDTreeNode updated = QKDTreeMakeNode(node->node_kind, dimensions,
										 node->nchildren, node->split_dim,
										 result.count, node->first_data_block,
										 result.sum_vec,
										 node->nchildren > 0 ? QKDTreeNodeBoundaries(node) : NULL,
										 node->nchildren > 0 ? QKDTreeNodeChildren(node) : NULL);

		QKDTreeRewritePage(index, MAIN_FORKNUM, block, QKDTREE_PAGE_NODE,
						   InvalidBlockNumber, updated,
						   QKDTreeNodeSize(dimensions, node->nchildren), true);
		pfree(updated);
	}

	pfree(node);
	return result;
}

IndexBulkDeleteResult *
qkdtreebulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				  IndexBulkDeleteCallback callback, void *callback_state)
{
	QKDTreeMetaPageData meta;
	QKDTreeVacuumResult result;

	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));

	QKDTreeReadMeta(info->index, &meta);
	LockPage(info->index, QKDTREE_METAPAGE_BLKNO, ExclusiveLock);
	result = VacuumNode(info->index, meta.root_block, meta.dimensions,
					stats, callback, callback_state);
	UnlockPage(info->index, QKDTREE_METAPAGE_BLKNO, ExclusiveLock);
	pfree(result.sum_vec);
	return stats;
}

IndexBulkDeleteResult *
qkdtreevacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (info->analyze_only)
		return stats;
	if (stats == NULL)
		return NULL;

	stats->num_pages = RelationGetNumberOfBlocks(info->index);
	return stats;
}
