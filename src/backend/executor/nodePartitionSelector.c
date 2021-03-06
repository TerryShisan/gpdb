/*-------------------------------------------------------------------------
 *
 * nodePartitionSelector.c
 *	  implement the execution of PartitionSelector for selecting partition
 *	  Oids based on a given set of predicates. It works for both constant
 *	  partition elimination and join partition elimination
 *
 * Copyright (c) 2014, Pivotal Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "cdb/cdbpartition.h"
#include "cdb/partitionselection.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodePartitionSelector.h"
#include "utils/memutils.h"

static void
ResetPrevSelParts(PartitionSelectorState *partSelState);

static void
partition_propagation(PartitionSelectorState *partSelState, SelectedParts *dynamicSelParts);

/* PartitionSelector Slots */
#define PARTITIONSELECTOR_NSLOTS 1

/* Return number of TupleTableSlots used by nodePartitionSelector.*/
int
ExecCountSlotsPartitionSelector(PartitionSelector *node)
{
	if (NULL != outerPlan(node))
	{
		return ExecCountSlotsNode(outerPlan(node)) + PARTITIONSELECTOR_NSLOTS;
	}
	return PARTITIONSELECTOR_NSLOTS;
}

void
initGpmonPktForPartitionSelector(Plan *planNode, gpmon_packet_t *gpmon_pkt, EState *estate)
{
	Assert(planNode != NULL && gpmon_pkt != NULL && IsA(planNode, PartitionSelector));

	InitPlanNodeGpmonPkt(planNode, gpmon_pkt, estate, PMNT_PartitionSelector,
							(int64)planNode->plan_rows,
							NULL);
}

/* ----------------------------------------------------------------
 *		ExecInitPartitionSelector
 *
 *		Create the run-time state information for PartitionSelector node
 *		produced by Orca and initializes outer child if exists.
 *
 * ----------------------------------------------------------------
 */
PartitionSelectorState *
ExecInitPartitionSelector(PartitionSelector *node, EState *estate, int eflags)
{
	/* check for unsupported flags */
	Assert (!(eflags & (EXEC_FLAG_MARK | EXEC_FLAG_BACKWARD)));

	PartitionSelectorState *psstate = initPartitionSelection(true /*isRunTime*/, node, estate);

	/* tuple table initialization */
	ExecInitResultTupleSlot(estate, &psstate->ps);
	ExecAssignResultTypeFromTL(&psstate->ps);
	ExecAssignProjectionInfo(&psstate->ps, NULL);

	/* initialize child nodes */
	/* No inner plan for PartitionSelector */
	Assert(NULL == innerPlan(node));
	if (NULL != outerPlan(node))
	{
		outerPlanState(psstate) = ExecInitNode(outerPlan(node), estate, eflags);
	}

	initGpmonPktForPartitionSelector((Plan *)node, &psstate->ps.gpmon_pkt, estate);

	return psstate;
}

/* ----------------------------------------------------------------
 *		ExecPartitionSelector(node)
 *
 *		Compute and propagate partition table Oids that will be
 *		used by Dynamic table scan. There are two ways of
 *		executing PartitionSelector.
 *
 *		1. Constant partition elimination
 *		Plan structure:
 *			Sequence
 *				|--PartitionSelector
 *				|--DynamicTableScan
 *		In this case, PartitionSelector evaluates constant partition
 *		constraints to compute and propagate partition table Oids.
 *		It only need to be called once.
 *
 *		2. Join partition elimination
 *		Plan structure:
 *			...:
 *				|--DynamicTableScan
 *				|--...
 *					|--PartitionSelector
 *						|--...
 *		In this case, PartitionSelector is in the same slice as
 *		DynamicTableScan, DynamicIndexScan or DynamicBitmapHeapScan.
 *		It is executed for each tuple coming from its child node.
 *		It evaluates partition constraints with the input tuple and
 *		propagate matched partition table Oids.
 *
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecPartitionSelector(PartitionSelectorState *node)
{
	PartitionSelector *ps = (PartitionSelector *) node->ps.plan;
	if (ps->staticSelection)
	{
		/* propagate the part oids obtained via static partition selection */
		partition_propagation(node, NULL /* No dynamically selected parts */);
		*node->acceptedLeafPart = NULL;
		return NULL;
	}

	/* Retrieve PartitionNode and access method from root table.
	 * We cannot do it during node initialization as
	 * DynamicTableScanInfo is not properly initialized yet.
	 */
	if (NULL == node->rootPartitionNode)
	{
		Assert(NULL != dynamicTableScanInfo);
		getPartitionNodeAndAccessMethod
									(
									ps->relid,
									dynamicTableScanInfo->partsMetadata,
									dynamicTableScanInfo->memoryContext,
									&node->rootPartitionNode,
									&node->accessMethods
									);
	}

	TupleTableSlot *inputSlot = NULL;
	if (NULL != outerPlanState(node))
	{
		/* Join partition elimination */
		/* get tuple from outer children */
		PlanState *outerPlan = outerPlanState(node);
		Assert(outerPlan);
		inputSlot = ExecProcNode(outerPlan);

		if (TupIsNull(inputSlot))
		{
			/* no more tuples from outerPlan */
			return NULL;
		}
	}

	/* partition elimination with the given input tuple */
	SelectedParts *selparts = processLevel(node, 0 /* level */, inputSlot);

	/* partition propagation */
	if (NULL != ps->propagationExpression)
	{
		partition_propagation(node, selparts);
	}

	TupleTableSlot *candidateOutputSlot = NULL;
	if (NULL != inputSlot)
	{
		ExprContext *econtext = node->ps.ps_ExprContext;
		ResetExprContext(econtext);
		node->ps.ps_OuterTupleSlot = inputSlot;
		econtext->ecxt_outertuple = inputSlot;
		econtext->ecxt_scantuple = inputSlot;

		ExprDoneCond isDone = ExprSingleResult;
		candidateOutputSlot = ExecProject(node->ps.ps_ProjInfo, &isDone);
		Assert (ExprSingleResult == isDone);
	}

	/* reset acceptedLeafPart */
	if (NULL != *node->acceptedLeafPart)
	{
		pfree(*node->acceptedLeafPart);
		*node->acceptedLeafPart = NULL;
	}
	return candidateOutputSlot;
}

/* ----------------------------------------------------------------
 *		ExecReScanPartitionSelector(node)
 *
 *		ExecReScan routine for PartitionSelector.
 * ----------------------------------------------------------------
 */
void
ExecReScanPartitionSelector(PartitionSelectorState *node, ExprContext *exprCtxt)
{
	/* reset PartitionSelectorState */
	PartitionSelector *ps = (PartitionSelector *) node->ps.plan;
	
	Assert (NULL == *node->acceptedLeafPart);
	
	for(int iter = 0; iter < ps->nLevels; iter++)
	{
		node->levelPartConstraints[iter] = NULL;
	}

	/* free result tuple slot */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * If we are being passed an outer tuple, link it into the "regular"
	 * per-tuple econtext for possible qual eval.
	 */
	if (exprCtxt != NULL)
	{
		ExprContext *stdecontext = node->ps.ps_ExprContext;
		stdecontext->ecxt_outertuple = exprCtxt->ecxt_outertuple;
	}

	/* If the PartitionSelector is in the inner side of a nest loop join,
	 * it should be constant partition elimination and thus has no child node.*/
#if USE_ASSERT_CHECKING
	PlanState  *outerPlan = outerPlanState(node);
	Assert (NULL == outerPlan);
#endif

}

/* ----------------------------------------------------------------
 *		ExecEndPartitionSelector(node)
 *
 *		ExecEnd routine for PartitionSelector. Free resources
 *		and clear tuple.
 *
 * ----------------------------------------------------------------
 */
void
ExecEndPartitionSelector(PartitionSelectorState *node)
{
	ExecFreeExprContext(&node->ps);

	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/* clean child node */
	if (NULL != outerPlanState(node))
	{
		ExecEndNode(outerPlanState(node));
	}

	ResetPrevSelParts(node);
	EndPlanStateGpmonPkt(&node->ps);
}

/* ----------------------------------------------------------------
 *		ResetPrevSelParts
 *
 *		Resets and frees previously selected partition list
 *
 * ----------------------------------------------------------------
 */
static void
ResetPrevSelParts(PartitionSelectorState *partSelState)
{
	SelectedParts *prevSelParts = partSelState->prevSelParts;

	if (NULL != prevSelParts)
	{
		list_free(prevSelParts->partOids);
		list_free(prevSelParts->scanIds);
		pfree(prevSelParts);

		partSelState->prevSelParts = NULL;
	}
}

/* ----------------------------------------------------------------
 *		UndoPrevPropagation
 *
 *		Undo propagated partitions in the previous run.
 *
 * ----------------------------------------------------------------
 */
static void
UndoPrevPropagation(PartitionSelectorState *partSelState)
{
	SelectedParts *prevSelParts = partSelState->prevSelParts;

	if (NULL != prevSelParts)
	{
		int32 selectorId = ((PartitionSelector *)partSelState->ps.plan)->selectorId;

		ListCell *lcOid = NULL;
		ListCell *lcScanId = NULL;
		forboth (lcOid, prevSelParts->partOids, lcScanId, prevSelParts->scanIds)
		{
			Oid partOid = lfirst_oid(lcOid);
			int scanId = lfirst_int(lcScanId);

			RemovePartSelectorForPartOid(scanId, partOid, selectorId);
		}

		ResetPrevSelParts(partSelState);
	}
}

/* ----------------------------------------------------------------
 *		partition_propagation
 *
 *		Propagate a list of leaf part Oids to the corresponding dynamic scans
 *
 * ----------------------------------------------------------------
 */
static void
partition_propagation(PartitionSelectorState *partSelState, SelectedParts *dynamicSelParts)
{
	PartitionSelector *partSel = (PartitionSelector *)partSelState->ps.plan;
	List *partOids = NULL;
	List *scanIds = NULL;

	if (NULL != dynamicSelParts)
	{
		UndoPrevPropagation(partSelState);
		partSelState->prevSelParts = dynamicSelParts;

		partOids = dynamicSelParts->partOids;
		scanIds = dynamicSelParts->scanIds;
	}
	else if (partSel->staticSelection)
	{
		partOids = partSel->staticPartOids;
		scanIds = partSel->staticScanIds;
	}

	int32 selectorId = partSel->selectorId;

	Assert (list_length(partOids) == list_length(scanIds));

	ListCell *lcOid = NULL;
	ListCell *lcScanId = NULL;
	forboth (lcOid, partOids, lcScanId, scanIds)
	{
		Oid partOid = lfirst_oid(lcOid);
		int scanId = lfirst_int(lcScanId);

		/* TODO: optimization to skip insertion of static parts multiple times */
		InsertPidIntoDynamicTableScanInfo(scanId, partOid, selectorId);
	}
}

/* EOF */
