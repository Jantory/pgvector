#ifndef QKDTREE_H
#define QKDTREE_H

#include "postgres.h"

#include "access/genam.h"
#include "commands/vacuum.h"
#include "nodes/execnodes.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/itemptr.h"
#include "utils/rel.h"

#include "intvec.h"

#define QKDTREE_MAGIC_NUMBER 0x514B4454U
#define QKDTREE_VERSION 1
#define QKDTREE_PAGE_ID 0x514B

#define QKDTREE_METAPAGE_BLKNO 0

#define QKDTREE_DEFAULT_BRANCH_FACTOR 4
#define QKDTREE_MIN_BRANCH_FACTOR 2
#define QKDTREE_MAX_BRANCH_FACTOR 32

#define QKDTREE_DEFAULT_LEAF_SIZE 32
#define QKDTREE_MIN_LEAF_SIZE 1
#define QKDTREE_MAX_LEAF_SIZE 4096

#define QKDTREE_DEFAULT_BEAM_WIDTH 8
#define QKDTREE_MIN_BEAM_WIDTH 1
#define QKDTREE_MAX_BEAM_WIDTH 65536

/* sum_vec for a 768-dimensional vector occupies 6 KiB */
#define QKDTREE_MAX_DIM 768

#define QKDTREE_DISTANCE_PROC 1

typedef enum QKDTreePageKind
{
	QKDTREE_PAGE_META = 1,
	QKDTREE_PAGE_NODE = 2,
	QKDTREE_PAGE_DATA = 3
} QKDTreePageKind;

typedef enum QKDTreeNodeKind
{
	QKDTREE_NODE_LEAF = 1,
	QKDTREE_NODE_INTERNAL = 2
} QKDTreeNodeKind;

typedef struct QKDTreeOptions
{
	int32		vl_len_;
	int		branch_factor;
	int		leaf_size;
} QKDTreeOptions;

typedef struct QKDTreeMetaPageData
{
	uint32		magic_number;
	uint32		version;
	uint16		dimensions;
	uint16		branch_factor;
	uint32		leaf_size;
	BlockNumber root_block;
} QKDTreeMetaPageData;

typedef struct QKDTreePageOpaqueData
{
	BlockNumber next_block;
	uint16		page_kind;
	uint16		page_id;
} QKDTreePageOpaqueData;

typedef QKDTreePageOpaqueData *QKDTreePageOpaque;

/*
 * A node is the only item on a node page. Variable data is laid out as:
 *
 *   int64 sum_vec[dimensions]
 *   int64 boundaries[nchildren - 1]       (internal nodes only)
 *   BlockNumber children[nchildren]       (internal nodes only)
 */
typedef struct QKDTreeNodeData
{
	uint16		node_kind;
	uint16		dimensions;
	uint16		nchildren;
	int16		split_dim;
	int64		count;
	BlockNumber first_data_block;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} QKDTreeNodeData;

typedef QKDTreeNodeData *QKDTreeNode;

/* A data-page item. Coordinates are stored compactly as signed int16. */
typedef struct QKDTreePointTupleData
{
	ItemPointerData heap_tid;
	int16		coords[FLEXIBLE_ARRAY_MEMBER];
} QKDTreePointTupleData;

typedef QKDTreePointTupleData *QKDTreePointTuple;

#define QKDTreePageGetOpaque(page) ((QKDTreePageOpaque) PageGetSpecialPointer(page))

Size		QKDTreeNodeSize(int dimensions, int nchildren);
Size		QKDTreePointSize(int dimensions);
int64	   *QKDTreeNodeSum(QKDTreeNode node);
int64	   *QKDTreeNodeBoundaries(QKDTreeNode node);
BlockNumber *QKDTreeNodeChildren(QKDTreeNode node);
void		QKDTreeCopyCoordinates(IntVector *vector, int dimensions, int16 *coords);
QKDTreeNode QKDTreeMakeNode(int node_kind, int dimensions, int nchildren,
								 int split_dim, int64 count,
								 BlockNumber first_data_block,
								 const int64 *sum_vec,
								 const int64 *boundaries,
								 const BlockNumber *children);
BlockNumber QKDTreeCreatePage(Relation index, ForkNumber fork_num,
								 int page_kind, BlockNumber next_block,
								 const void *item, Size item_size,
								 bool use_wal);
void		QKDTreeRewritePage(Relation index, ForkNumber fork_num,
								 BlockNumber block, int page_kind,
								 BlockNumber next_block, const void *item,
								 Size item_size, bool use_wal);
QKDTreeNode QKDTreeReadNode(Relation index, BlockNumber block);
void		QKDTreeReadMeta(Relation index, QKDTreeMetaPageData *meta);

extern int qkdtree_beam_width;

void		QKDTreeInit(void);

IndexBuildResult *qkdtreebuild(Relation heap, Relation index, IndexInfo *index_info);
void		qkdtreebuildempty(Relation index);
bool		qkdtreeinsert(Relation index, Datum *values, bool *isnull,
						 ItemPointer heap_tid, Relation heap,
						 IndexUniqueCheck check_unique
#if PG_VERSION_NUM >= 140000
						 , bool index_unchanged
#endif
						 , IndexInfo *index_info);
IndexBulkDeleteResult *qkdtreebulkdelete(IndexVacuumInfo *info,
											IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback,
											void *callback_state);
IndexBulkDeleteResult *qkdtreevacuumcleanup(IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats);
IndexScanDesc qkdtreebeginscan(Relation index, int nkeys, int norderbys);
void		qkdtreerescan(IndexScanDesc scan, ScanKey keys, int nkeys,
					  ScanKey orderbys, int norderbys);
bool		qkdtreegettuple(IndexScanDesc scan, ScanDirection dir);
void		qkdtreeendscan(IndexScanDesc scan);

#endif
