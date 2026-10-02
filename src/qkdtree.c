#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/reloptions.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "qkdtree.h"
#include "utils/guc.h"
#include "utils/float.h"
#include "utils/relcache.h"
#include "utils/selfuncs.h"

#if PG_VERSION_NUM < 150000
#define MarkGUCPrefixReserved(x) EmitWarningsOnPlaceholders(x)
#endif

int			qkdtree_beam_width;
static relopt_kind qkdtree_relopt_kind;

void
QKDTreeInit(void)
{
	qkdtree_relopt_kind = add_reloption_kind();
	add_int_reloption(qkdtree_relopt_kind, "branch_factor",
					  "Maximum number of children per internal node",
					  QKDTREE_DEFAULT_BRANCH_FACTOR,
					  QKDTREE_MIN_BRANCH_FACTOR,
					  QKDTREE_MAX_BRANCH_FACTOR,
					  AccessExclusiveLock);
	add_int_reloption(qkdtree_relopt_kind, "leaf_size",
					  "Maximum number of points stored in a leaf",
					  QKDTREE_DEFAULT_LEAF_SIZE,
					  QKDTREE_MIN_LEAF_SIZE,
					  QKDTREE_MAX_LEAF_SIZE,
					  AccessExclusiveLock);

	DefineCustomIntVariable("qkdtree.beam_width",
						"Sets the number of tree nodes retained at each search level",
						NULL,
						&qkdtree_beam_width,
						QKDTREE_DEFAULT_BEAM_WIDTH,
						QKDTREE_MIN_BEAM_WIDTH,
						QKDTREE_MAX_BEAM_WIDTH,
						PGC_USERSET, 0, NULL, NULL, NULL);

	MarkGUCPrefixReserved("qkdtree");
}

static void
qkdtreecostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					Cost *index_startup_cost, Cost *index_total_cost,
					Selectivity *index_selectivity, double *index_correlation,
					double *index_pages)
{
	GenericCosts costs;

	if (path->indexorderbys == NIL)
	{
		*index_startup_cost = get_float8_infinity();
		*index_total_cost = get_float8_infinity();
		*index_selectivity = 0;
		*index_correlation = 0;
		*index_pages = 0;
#if PG_VERSION_NUM >= 180000
		path->path.disabled_nodes = 2;
#endif
		return;
	}

	MemSet(&costs, 0, sizeof(costs));
	genericcostestimate(root, path, loop_count, &costs);

	/* Most work is performed before the first tuple is returned. */
	costs.indexStartupCost = costs.indexTotalCost * 0.1;

	*index_startup_cost = costs.indexStartupCost;
	*index_total_cost = costs.indexTotalCost;
	*index_selectivity = costs.indexSelectivity;
	*index_correlation = costs.indexCorrelation;
	*index_pages = costs.numIndexPages;
}

static bytea *
qkdtreeoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"branch_factor", RELOPT_TYPE_INT, offsetof(QKDTreeOptions, branch_factor)},
		{"leaf_size", RELOPT_TYPE_INT, offsetof(QKDTreeOptions, leaf_size)},
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  qkdtree_relopt_kind,
									  sizeof(QKDTreeOptions),
									  tab, lengthof(tab));
}

static bool
qkdtreevalidate(Oid opclassoid)
{
	return true;
}

FUNCTION_PREFIX PG_FUNCTION_INFO_V1(qkdtreehandler);
Datum
qkdtreehandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 0;
	amroutine->amsupport = 1;
#if PG_VERSION_NUM >= 180000
	amroutine->amoptsprocnum = 0;
#endif
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = true;
#if PG_VERSION_NUM >= 180000
	amroutine->amcanhash = false;
	amroutine->amconsistentequality = false;
	amroutine->amconsistentordering = false;
#endif
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
#if PG_VERSION_NUM >= 170000
	amroutine->amcanbuildparallel = false;
#endif
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
#if PG_VERSION_NUM >= 160000
	amroutine->amsummarizing = false;
#endif
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = qkdtreebuild;
	amroutine->ambuildempty = qkdtreebuildempty;
	amroutine->aminsert = qkdtreeinsert;
#if PG_VERSION_NUM >= 170000
	amroutine->aminsertcleanup = NULL;
#endif
	amroutine->ambulkdelete = qkdtreebulkdelete;
	amroutine->amvacuumcleanup = qkdtreevacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = qkdtreecostestimate;
#if PG_VERSION_NUM >= 180000
	amroutine->amgettreeheight = NULL;
#endif
	amroutine->amoptions = qkdtreeoptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = qkdtreevalidate;
#if PG_VERSION_NUM >= 140000
	amroutine->amadjustmembers = NULL;
#endif
	amroutine->ambeginscan = qkdtreebeginscan;
	amroutine->amrescan = qkdtreerescan;
	amroutine->amgettuple = qkdtreegettuple;
	amroutine->amgetbitmap = NULL;
	amroutine->amendscan = qkdtreeendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;
#if PG_VERSION_NUM >= 180000
	amroutine->amtranslatestrategy = NULL;
	amroutine->amtranslatecmptype = NULL;
#endif

	PG_RETURN_POINTER(amroutine);
}
