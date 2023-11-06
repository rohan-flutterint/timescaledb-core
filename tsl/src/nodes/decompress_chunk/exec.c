/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#include <postgres.h>
#include <miscadmin.h>
#include <access/sysattr.h>
#include <executor/executor.h>
#include <nodes/bitmapset.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <optimizer/optimizer.h>
#include <parser/parsetree.h>
#include <rewrite/rewriteManip.h>
#include <utils/datum.h>
#include <utils/memutils.h>
#include <utils/typcache.h>

#include "compat/compat.h"
#include "compression/array.h"
#include "compression/arrow_c_data_interface.h"
#include "compression/compression.h"
#include "guc.h"
#include "import/ts_explain.h"
#include "nodes/decompress_chunk/batch_array.h"
#include "nodes/decompress_chunk/batch_queue_fifo.h"
#include "nodes/decompress_chunk/batch_queue_heap.h"
#include "nodes/decompress_chunk/decompress_chunk.h"
#include "nodes/decompress_chunk/exec.h"
#include "nodes/decompress_chunk/planner.h"
#include "ts_catalog/hypertable_compression.h"

static void decompress_chunk_begin(CustomScanState *node, EState *estate, int eflags);
static void decompress_chunk_end(CustomScanState *node);
static void decompress_chunk_rescan(CustomScanState *node);
static void decompress_chunk_explain(CustomScanState *node, List *ancestors, ExplainState *es);

static CustomExecMethods decompress_chunk_state_methods = {
	.BeginCustomScan = decompress_chunk_begin,
	.ExecCustomScan = NULL, /* To be determined later. */
	.EndCustomScan = decompress_chunk_end,
	.ReScanCustomScan = decompress_chunk_rescan,
	.ExplainCustomScan = decompress_chunk_explain,
};

struct BatchQueueFunctions
{
	void (*create)(DecompressChunkState *);
	void (*free)(DecompressChunkState *);
	bool (*needs_next_batch)(DecompressChunkState *);
	void (*pop)(DecompressChunkState *);
	void (*push_batch)(DecompressChunkState *, TupleTableSlot *);
	void (*reset)(DecompressChunkState *);
	TupleTableSlot *(*top_tuple)(DecompressChunkState *);
};

static const struct BatchQueueFunctions BatchQueueFunctionsFifo = {
	.create = batch_queue_fifo_create,
	.free = batch_queue_fifo_free,
	.needs_next_batch = batch_queue_fifo_needs_next_batch,
	.pop = batch_queue_fifo_pop,
	.push_batch = batch_queue_fifo_push_batch,
	.reset = batch_queue_fifo_reset,
	.top_tuple = batch_queue_fifo_top_tuple,
};

static const struct BatchQueueFunctions BatchQueueFunctionsHeap = {
	.create = batch_queue_heap_create,
	.free = batch_queue_heap_free,
	.needs_next_batch = batch_queue_heap_needs_next_batch,
	.pop = batch_queue_heap_pop,
	.push_batch = batch_queue_heap_push_batch,
	.reset = batch_queue_heap_reset,
	.top_tuple = batch_queue_heap_top_tuple,
};

/*
 * Build the sortkeys data structure from the list structure in the
 * custom_private field of the custom scan. This sort info is used to sort
 * binary heap used for sorted merge append.
 */
static void
build_batch_sorted_merge_info(DecompressChunkState *chunk_state)
{
	List *sortinfo = chunk_state->sortinfo;
	if (sortinfo == NIL)
	{
		chunk_state->n_sortkeys = 0;
		chunk_state->sortkeys = NULL;
		return;
	}

	List *sort_col_idx = linitial(sortinfo);
	List *sort_ops = lsecond(sortinfo);
	List *sort_collations = lthird(sortinfo);
	List *sort_nulls = lfourth(sortinfo);

	chunk_state->n_sortkeys = list_length(linitial((sortinfo)));

	Assert(list_length(sort_col_idx) == list_length(sort_ops));
	Assert(list_length(sort_ops) == list_length(sort_collations));
	Assert(list_length(sort_collations) == list_length(sort_nulls));
	Assert(chunk_state->n_sortkeys > 0);

	SortSupportData *sortkeys = palloc0(sizeof(SortSupportData) * chunk_state->n_sortkeys);

	/* Inspired by nodeMergeAppend.c */
	for (int i = 0; i < chunk_state->n_sortkeys; i++)
	{
		SortSupportData *sortKey = &sortkeys[i];

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = list_nth_oid(sort_collations, i);
		sortKey->ssup_nulls_first = list_nth_oid(sort_nulls, i);
		sortKey->ssup_attno = list_nth_oid(sort_col_idx, i);

		/*
		 * It isn't feasible to perform abbreviated key conversion, since
		 * tuples are pulled into mergestate's binary heap as needed.  It
		 * would likely be counter-productive to convert tuples into an
		 * abbreviated representation as they're pulled up, so opt out of that
		 * additional optimization entirely.
		 */
		sortKey->abbreviate = false;

		PrepareSortSupportFromOrderingOp(list_nth_oid(sort_ops, i), sortKey);
	}

	chunk_state->sortkeys = sortkeys;
}

Node *
decompress_chunk_state_create(CustomScan *cscan)
{
	DecompressChunkState *chunk_state;

	chunk_state = (DecompressChunkState *) newNode(sizeof(DecompressChunkState), T_CustomScanState);

	chunk_state->exec_methods = decompress_chunk_state_methods;
	chunk_state->csstate.methods = &chunk_state->exec_methods;

	Assert(IsA(cscan->custom_private, List));
	Assert(list_length(cscan->custom_private) == 6);
	List *settings = linitial(cscan->custom_private);
	chunk_state->decompression_map = lsecond(cscan->custom_private);
	chunk_state->is_segmentby_column = lthird(cscan->custom_private);
	chunk_state->bulk_decompression_column = lfourth(cscan->custom_private);
	chunk_state->vectorized_aggregation_column = lfifth(cscan->custom_private);
	chunk_state->sortinfo = lsixth(cscan->custom_private);
	chunk_state->custom_scan_tlist = cscan->custom_scan_tlist;

	Assert(IsA(settings, IntList));
	Assert(list_length(settings) == 6);
	chunk_state->hypertable_id = linitial_int(settings);
	chunk_state->chunk_relid = lsecond_int(settings);
	chunk_state->reverse = lthird_int(settings);
	chunk_state->batch_sorted_merge = lfourth_int(settings);
	chunk_state->enable_bulk_decompression = lfifth_int(settings);
	chunk_state->perform_vectorized_aggregation = lsixth_int(settings);

	Assert(IsA(cscan->custom_exprs, List));
	Assert(list_length(cscan->custom_exprs) == 1);
	chunk_state->vectorized_quals_original = linitial(cscan->custom_exprs);
	Assert(list_length(chunk_state->decompression_map) ==
		   list_length(chunk_state->is_segmentby_column));

#ifdef USE_ASSERT_CHECKING
	if (chunk_state->perform_vectorized_aggregation)
	{
		Assert(list_length(chunk_state->decompression_map) ==
			   list_length(chunk_state->vectorized_aggregation_column));
	}
#endif

	return (Node *) chunk_state;
}

typedef struct ConstifyTableOidContext
{
	Index chunk_index;
	Oid chunk_relid;
	bool made_changes;
} ConstifyTableOidContext;

static Node *
constify_tableoid_walker(Node *node, ConstifyTableOidContext *ctx)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, Var))
	{
		Var *var = castNode(Var, node);

		if ((Index) var->varno != ctx->chunk_index)
			return node;

		if (var->varattno == TableOidAttributeNumber)
		{
			ctx->made_changes = true;
			return (
				Node *) makeConst(OIDOID, -1, InvalidOid, 4, (Datum) ctx->chunk_relid, false, true);
		}

		/*
		 * we doublecheck system columns here because projection will
		 * segfault if any system columns get through
		 */
		if (var->varattno < SelfItemPointerAttributeNumber)
			elog(ERROR, "transparent decompression only supports tableoid system column");

		return node;
	}

	return expression_tree_mutator(node, constify_tableoid_walker, (void *) ctx);
}

static List *
constify_tableoid(List *node, Index chunk_index, Oid chunk_relid)
{
	ConstifyTableOidContext ctx = {
		.chunk_index = chunk_index,
		.chunk_relid = chunk_relid,
		.made_changes = false,
	};

	List *result = (List *) constify_tableoid_walker((Node *) node, &ctx);
	if (ctx.made_changes)
	{
		return result;
	}

	return node;
}

pg_attribute_always_inline static TupleTableSlot *
decompress_chunk_exec_impl(DecompressChunkState *chunk_state,
						   const struct BatchQueueFunctions *queue);

static TupleTableSlot *
decompress_chunk_exec_fifo(CustomScanState *node)
{
	DecompressChunkState *chunk_state = (DecompressChunkState *) node;
	Assert(!chunk_state->batch_sorted_merge);
	return decompress_chunk_exec_impl(chunk_state, &BatchQueueFunctionsFifo);
}

static TupleTableSlot *
decompress_chunk_exec_heap(CustomScanState *node)
{
	DecompressChunkState *chunk_state = (DecompressChunkState *) node;
	Assert(chunk_state->batch_sorted_merge);
	return decompress_chunk_exec_impl(chunk_state, &BatchQueueFunctionsHeap);
}

/*
 * Complete initialization of the supplied CustomScanState.
 *
 * Standard fields have been initialized by ExecInitCustomScan,
 * but any private fields should be initialized here.
 */
static void
decompress_chunk_begin(CustomScanState *node, EState *estate, int eflags)
{
	DecompressChunkState *chunk_state = (DecompressChunkState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	Plan *compressed_scan = linitial(cscan->custom_plans);
	Assert(list_length(cscan->custom_plans) == 1);

	PlanState *ps = &node->ss.ps;
	if (ps->ps_ProjInfo)
	{
		/*
		 * if we are projecting we need to constify tableoid references here
		 * because decompressed tuple are virtual tuples and don't have
		 * system columns.
		 *
		 * We do the constify in executor because even after plan creation
		 * our targetlist might still get modified by parent nodes pushing
		 * down targetlist.
		 */
		List *tlist = ps->plan->targetlist;
		List *modified_tlist =
			constify_tableoid(tlist, cscan->scan.scanrelid, chunk_state->chunk_relid);

		if (modified_tlist != tlist)
		{
			ps->ps_ProjInfo =
				ExecBuildProjectionInfo(modified_tlist,
										ps->ps_ExprContext,
										ps->ps_ResultTupleSlot,
										ps,
										node->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
		}
	}

	/* Extract sort info */
	build_batch_sorted_merge_info(chunk_state);
	/* Sort keys should only be present when sorted_merge_append is used */
	Assert(chunk_state->batch_sorted_merge == true || chunk_state->n_sortkeys == 0);
	Assert(chunk_state->n_sortkeys == 0 || chunk_state->sortkeys != NULL);

	/*
	 * Init the underlying compressed scan.
	 */
	node->custom_ps = lappend(node->custom_ps, ExecInitNode(compressed_scan, estate, eflags));

	/*
	 * Determine which columns we are going to decompress. Since in the hottest
	 * loop we work only with compressed columns, we'll put them in front of the
	 * array. So first, count how many compressed and not compressed columns
	 * we have.
	 */
	int num_compressed = 0;
	int num_total = 0;

	ListCell *dest_cell;
	ListCell *is_segmentby_cell;

	forboth (dest_cell,
			 chunk_state->decompression_map,
			 is_segmentby_cell,
			 chunk_state->is_segmentby_column)
	{
		AttrNumber output_attno = lfirst_int(dest_cell);
		if (output_attno == 0)
		{
			/* We are asked not to decompress this column, skip it. */
			continue;
		}

		if (output_attno > 0 && !lfirst_int(is_segmentby_cell))
		{
			/*
			 * Not a metadata column and not a segmentby column, hence a
			 * compressed one.
			 */
			num_compressed++;
		}

		num_total++;
	}

	Assert(num_compressed <= num_total);
	chunk_state->num_compressed_columns = num_compressed;
	chunk_state->num_total_columns = num_total;
	chunk_state->template_columns = palloc0(sizeof(DecompressChunkColumnDescription) * num_total);

	TupleDesc desc = chunk_state->csstate.ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	/*
	 * Compressed columns go in front, and the rest go to the back, so we have
	 * separate indices for them.
	 */
	int current_compressed = 0;
	int current_not_compressed = num_compressed;
	for (int compressed_index = 0; compressed_index < list_length(chunk_state->decompression_map);
		 compressed_index++)
	{
		DecompressChunkColumnDescription column = {
			.compressed_scan_attno = AttrOffsetGetAttrNumber(compressed_index),
			.output_attno = list_nth_int(chunk_state->decompression_map, compressed_index),
			.bulk_decompression_supported =
				list_nth_int(chunk_state->bulk_decompression_column, compressed_index)
		};

		if (column.output_attno == 0)
		{
			/* We are asked not to decompress this column, skip it. */
			continue;
		}

		if (column.output_attno > 0)
		{
			if (chunk_state->perform_vectorized_aggregation &&
				lfirst_int(list_nth_cell(chunk_state->vectorized_aggregation_column,
										 compressed_index)) != -1)
			{
				column.typid = lfirst_int(
					list_nth_cell(chunk_state->vectorized_aggregation_column, compressed_index));
			}
			else
			{
				/* normal column that is also present in decompressed chunk */
				Form_pg_attribute attribute =
					TupleDescAttr(desc, AttrNumberGetAttrOffset(column.output_attno));

				column.typid = attribute->atttypid;
				column.value_bytes = get_typlen(column.typid);
			}

			if (list_nth_int(chunk_state->is_segmentby_column, compressed_index))
				column.type = SEGMENTBY_COLUMN;
			else
				column.type = COMPRESSED_COLUMN;
		}
		else
		{
			/* metadata columns */
			switch (column.output_attno)
			{
				case DECOMPRESS_CHUNK_COUNT_ID:
					column.type = COUNT_COLUMN;
					break;
				case DECOMPRESS_CHUNK_SEQUENCE_NUM_ID:
					column.type = SEQUENCE_NUM_COLUMN;
					break;
				default:
					elog(ERROR, "Invalid column attno \"%d\"", column.output_attno);
					break;
			}
		}

		if (column.type == COMPRESSED_COLUMN)
		{
			Assert(current_compressed < num_total);
			chunk_state->template_columns[current_compressed++] = column;
		}
		else
		{
			Assert(current_not_compressed < num_total);
			chunk_state->template_columns[current_not_compressed++] = column;
		}
	}

	Assert(current_compressed == num_compressed);
	Assert(current_not_compressed == num_total);

	chunk_state->n_batch_state_bytes =
		sizeof(DecompressBatchState) +
		sizeof(CompressedColumnValues) * chunk_state->num_compressed_columns;

	/*
	 * Calculate the desired size of the batch memory context. Especially if we
	 * use bulk decompression, the results should fit into the first page of the
	 * context, otherwise it's going to do malloc/free on every
	 * MemoryContextReset.
	 *
	 * Start with the default size.
	 */
	chunk_state->batch_memory_context_bytes = ALLOCSET_DEFAULT_INITSIZE;
	if (chunk_state->enable_bulk_decompression)
	{
		for (int i = 0; i < num_total; i++)
		{
			DecompressChunkColumnDescription *column = &chunk_state->template_columns[i];
			if (column->bulk_decompression_supported)
			{
				/* Values array, with 64 element padding (actually we have less). */
				chunk_state->batch_memory_context_bytes +=
					(GLOBAL_MAX_ROWS_PER_COMPRESSION + 64) * column->value_bytes;
				/* Also nulls bitmap. */
				chunk_state->batch_memory_context_bytes +=
					GLOBAL_MAX_ROWS_PER_COMPRESSION / (64 * sizeof(uint64));
				/* Arrow data structure. */
				chunk_state->batch_memory_context_bytes +=
					sizeof(ArrowArray) + sizeof(void *) * 2 /* buffers */;
				/* Memory context header overhead for the above parts. */
				chunk_state->batch_memory_context_bytes += sizeof(void *) * 3;
			}
		}
	}

	/* Round up to even number of 4k pages. */
	chunk_state->batch_memory_context_bytes =
		((chunk_state->batch_memory_context_bytes + 4095) / 4096) * 4096;

	/* As a precaution, limit it to 1MB. */
	chunk_state->batch_memory_context_bytes =
		Min(chunk_state->batch_memory_context_bytes, 1 * 1024 * 1024);

	elog(DEBUG3,
		 "Batch memory context has initial capacity of  %d bytes",
		 chunk_state->batch_memory_context_bytes);

	/*
	 * Choose which batch queue we are going to use: heap for batch sorted
	 * merge, and one-element FIFO for normal decompression.
	 */
	if (chunk_state->batch_sorted_merge)
	{
		chunk_state->batch_queue = &BatchQueueFunctionsHeap;
		chunk_state->exec_methods.ExecCustomScan = decompress_chunk_exec_heap;
	}
	else
	{
		chunk_state->batch_queue = &BatchQueueFunctionsFifo;
		chunk_state->exec_methods.ExecCustomScan = decompress_chunk_exec_fifo;
	}

	chunk_state->batch_queue->create(chunk_state);

	if (ts_guc_debug_require_batch_sorted_merge && !chunk_state->batch_sorted_merge)
	{
		elog(ERROR, "debug: batch sorted merge is required but not used");
	}

	/* Constify stable expressions in vectorized predicates. */
	chunk_state->have_constant_false_vectorized_qual = false;
	PlannerGlobal glob = {
		.boundParams = node->ss.ps.state->es_param_list_info,
	};
	PlannerInfo root = {
		.glob = &glob,
	};
	ListCell *lc;
	foreach (lc, chunk_state->vectorized_quals_original)
	{
		Node *constified = estimate_expression_value(&root, (Node *) lfirst(lc));

		/*
		 * Note that some expressions are evaluated to a null Const, like a
		 * strict comparison with stable expression that evaluates to null. If
		 * we have such filter, no rows can pass, so we set a special flag to
		 * return early.
		 */
		if (IsA(constified, Const))
		{
			Const *c = castNode(Const, constified);
			if (c->constisnull || !DatumGetBool(c))
			{
				chunk_state->have_constant_false_vectorized_qual = true;
				break;
			}
			else
			{
				/*
				 * This is a constant true qual, every row passes and we can
				 * just ignore it. No idea how it can happen though.
				 */
				Assert(false);
				continue;
			}
		}

		OpExpr *opexpr = castNode(OpExpr, constified);
		Ensure(IsA(lsecond(opexpr->args), Const),
			   "failed to evaluate runtime constant in vectorized filter");
		chunk_state->vectorized_quals_constified =
			lappend(chunk_state->vectorized_quals_constified, constified);
	}
}

/*
 * Perform a vectorized aggregation on int4 values
 */
static TupleTableSlot *
perform_vectorized_sum_int4(DecompressChunkState *chunk_state, Aggref *aggref)
{
	Assert(chunk_state != NULL);
	Assert(aggref != NULL);

	/* Partial result is a int8 */
	Assert(aggref->aggtranstype == INT8OID);

	/* Two columns are decompressed, the column that needs to be aggregated and the count column */
	Assert(chunk_state->num_total_columns == 2);

	DecompressChunkColumnDescription *column_description = &chunk_state->template_columns[0];
	Assert(chunk_state->template_columns[1].type == COUNT_COLUMN);

	/* Get a free batch slot */
	const int new_batch_index = batch_array_get_free_slot(chunk_state);

	/* Nobody else should use batch states */
	Assert(new_batch_index == 0);
	DecompressBatchState *batch_state = batch_array_get_at(chunk_state, new_batch_index);

	/* Init per batch memory context */
	Assert(batch_state != NULL);
	init_per_batch_mctx(chunk_state, batch_state);
	Assert(batch_state->per_batch_context != NULL);

	/* Init bulk decompression memory context */
	init_bulk_decompression_mctx(chunk_state, CurrentMemoryContext);

	/* Get a reference the the output TupleTableSlot */
	TupleTableSlot *decompressed_scan_slot = chunk_state->csstate.ss.ss_ScanTupleSlot;
	Assert(decompressed_scan_slot->tts_tupleDescriptor->natts == 1);

	/* Set all attributes of the result tuple to NULL. So, we return NULL if no data is processed
	 * by our implementation. In addition, the call marks the slot as beeing used (i.e., no
	 * ExecStoreVirtualTuple call is required). */
	ExecStoreAllNullTuple(decompressed_scan_slot);
	Assert(!TupIsNull(decompressed_scan_slot));

	int64 result_sum = 0;

	if (column_description->type == SEGMENTBY_COLUMN)
	{
		/*
		 * To calculate the sum for a segment by value, we need to multiply the value of the segment
		 * by column with the number of compressed tuples in this batch.
		 */
		DecompressChunkColumnDescription *column_description_count =
			&chunk_state->template_columns[1];

		while (true)
		{
			TupleTableSlot *compressed_slot =
				ExecProcNode(linitial(chunk_state->csstate.custom_ps));

			if (TupIsNull(compressed_slot))
			{
				/* All segment by values are processed. */
				break;
			}

			bool isnull_value, isnull_elements;
			Datum value = slot_getattr(compressed_slot,
									   column_description->compressed_scan_attno,
									   &isnull_value);

			/* We have multiple compressed tuples for this segment by value. Get number of
			 * compressed tuples */
			Datum elements = slot_getattr(compressed_slot,
										  column_description_count->compressed_scan_attno,
										  &isnull_elements);

			if (!isnull_value && !isnull_elements)
			{
				int32 intvalue = DatumGetInt32(value);
				int32 amount = DatumGetInt32(elements);
				int64 batch_sum = 0;

				Assert(amount > 0);

				/* We have at least one value */
				decompressed_scan_slot->tts_isnull[0] = false;

				/* Multiply the number of tuples with the actual value */
				if (unlikely(pg_mul_s64_overflow(intvalue, amount, &batch_sum)))
				{
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("bigint out of range")));
				}

				/* Add the value to our sum */
				if (unlikely(pg_add_s64_overflow(result_sum, batch_sum, ((int64 *) &result_sum))))
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("bigint out of range")));
			}
		}
	}
	else if (column_description->type == COMPRESSED_COLUMN)
	{
		Assert(chunk_state->enable_bulk_decompression);
		Assert(column_description->bulk_decompression_supported);
		Assert(list_length(aggref->args) == 1);

		while (true)
		{
			TupleTableSlot *compressed_slot =
				ExecProcNode(linitial(chunk_state->csstate.custom_ps));
			if (TupIsNull(compressed_slot))
			{
				/* All compressed batches are processed. */
				break;
			}

			/* Decompress data */
			bool isnull;
			Datum value =
				slot_getattr(compressed_slot, column_description->compressed_scan_attno, &isnull);

			Ensure(isnull == false, "got unexpected NULL attribute value from compressed batch");

			/* We have at least one value */
			decompressed_scan_slot->tts_isnull[0] = false;

			CompressedDataHeader *header = (CompressedDataHeader *) PG_DETOAST_DATUM(value);
			ArrowArray *arrow = NULL;

			DecompressAllFunction decompress_all =
				tsl_get_decompress_all_function(header->compression_algorithm);
			Assert(decompress_all != NULL);

			MemoryContext context_before_decompression =
				MemoryContextSwitchTo(chunk_state->bulk_decompression_context);

			arrow = decompress_all(PointerGetDatum(header),
								   column_description->typid,
								   batch_state->per_batch_context);

			Assert(arrow != NULL);

			MemoryContextReset(chunk_state->bulk_decompression_context);
			MemoryContextSwitchTo(context_before_decompression);

			/* A compressed batch consists of 1000 tuples (see MAX_ROWS_PER_COMPRESSION). The
			 * attribute value is a int32 with a max value of 2^32. Even if all tuples have the max
			 * value, the max sum is = 1000 * 2^32 < 2^10 * 2^32 = 2^42. This is smaller than 2^64,
			 * which is the max value of the int64 variable. The same is true for negative values).
			 * Therefore, we don't need to check for overflows within the loop, which would slow
			 * down the calculation. */
			Assert(arrow->length <= MAX_ROWS_PER_COMPRESSION);
			Assert(MAX_ROWS_PER_COMPRESSION <= 1024);

			int64 batch_sum = 0;
			for (int i = 0; i < arrow->length; i++)
			{
				const bool arrow_isnull = !arrow_row_is_valid(arrow->buffers[0], i);

				if (likely(!arrow_isnull))
				{
					const int32 arrow_value = ((int32 *) arrow->buffers[1])[i];
					batch_sum += arrow_value;
				}
			}

			if (unlikely(pg_add_s64_overflow(result_sum, batch_sum, ((int64 *) &result_sum))))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("bigint out of range")));
		}
	}
	else
	{
		elog(ERROR, "unsupported column type");
	}

	/* Use Int64GetDatum to store the result since a 64-bit value is not pass-by-value on 32-bit
	 * systems */
	decompressed_scan_slot->tts_values[0] = Int64GetDatum(result_sum);

	return decompressed_scan_slot;
}

/*
 * Directly execute an aggregation function on decompressed data and emit a partial aggregate
 * result.
 *
 * Executing the aggregation directly in this node makes it possible to use the columnar data
 * directly before it is converted into row-based tuples.
 */
static TupleTableSlot *
perform_vectorized_aggregation(DecompressChunkState *chunk_state)
{
	Assert(list_length(chunk_state->custom_scan_tlist) == 1);

	/* Checked by planner */
	Assert(ts_guc_enable_vectorized_aggregation);
	Assert(ts_guc_enable_bulk_decompression);

	/* When using vectorized aggregates, only one result tuple is produced. So, if we have
	 * already initialized a batch state, the aggregation was already performed.
	 */
	if (bms_num_members(chunk_state->unused_batch_states) != chunk_state->n_batch_states)
	{
		ExecClearTuple(chunk_state->csstate.ss.ss_ScanTupleSlot);
		return chunk_state->csstate.ss.ss_ScanTupleSlot;
	}

	/* Determine which kind of vectorized aggregation we should perform */
	TargetEntry *tlentry = (TargetEntry *) linitial(chunk_state->custom_scan_tlist);
	Assert(IsA(tlentry->expr, Aggref));
	Aggref *aggref = castNode(Aggref, tlentry->expr);

	/* The aggregate should be a partial aggregate */
	Assert(aggref->aggsplit == AGGSPLIT_INITIAL_SERIAL);

	switch (aggref->aggfnoid)
	{
		case F_SUM_INT4:
			return perform_vectorized_sum_int4(chunk_state, aggref);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("vectorized aggregation for function %d is not supported",
							aggref->aggfnoid)));
			pg_unreachable();
	}
}

/*
 * The exec function for the DecompressChunk node. It takes the explicit queue
 * functions pointer as an optimization, to allow these functions to be
 * inlined in the FIFO case. This is important because this is a part of a
 * relatively hot loop.
 */
pg_attribute_always_inline static TupleTableSlot *
decompress_chunk_exec_impl(DecompressChunkState *chunk_state,
						   const struct BatchQueueFunctions *queue)
{
	if (chunk_state->perform_vectorized_aggregation)
	{
		return perform_vectorized_aggregation(chunk_state);
	}

	if (chunk_state->have_constant_false_vectorized_qual)
	{
		return NULL;
	}

	queue->pop(chunk_state);
	while (queue->needs_next_batch(chunk_state))
	{
		TupleTableSlot *subslot = ExecProcNode(linitial(chunk_state->csstate.custom_ps));
		if (TupIsNull(subslot))
		{
			/* Won't have more compressed tuples. */
			break;
		}

		queue->push_batch(chunk_state, subslot);
	}
	TupleTableSlot *result_slot = queue->top_tuple(chunk_state);

	if (TupIsNull(result_slot))
	{
		return NULL;
	}

	if (chunk_state->csstate.ss.ps.ps_ProjInfo)
	{
		ExprContext *econtext = chunk_state->csstate.ss.ps.ps_ExprContext;
		econtext->ecxt_scantuple = result_slot;
		return ExecProject(chunk_state->csstate.ss.ps.ps_ProjInfo);
	}

	return result_slot;
}

static void
decompress_chunk_rescan(CustomScanState *node)
{
	DecompressChunkState *chunk_state = (DecompressChunkState *) node;

	chunk_state->batch_queue->reset(chunk_state);

	for (int i = 0; i < chunk_state->n_batch_states; i++)
	{
		batch_array_free_at(chunk_state, i);
	}

	Assert(bms_num_members(chunk_state->unused_batch_states) == chunk_state->n_batch_states);

	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(linitial(node->custom_ps), node->ss.ps.chgParam);

	ExecReScan(linitial(node->custom_ps));
}

/* End the decompress operation and free the requested resources */
static void
decompress_chunk_end(CustomScanState *node)
{
	DecompressChunkState *chunk_state = (DecompressChunkState *) node;

	chunk_state->batch_queue->free(chunk_state);

	ExecEndNode(linitial(node->custom_ps));
}

/*
 * Output additional information for EXPLAIN of a custom-scan plan node.
 */
static void
decompress_chunk_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	DecompressChunkState *chunk_state = (DecompressChunkState *) node;

	ts_show_scan_qual(chunk_state->vectorized_quals_original,
					  "Vectorized Filter",
					  &node->ss.ps,
					  ancestors,
					  es);

	if (!node->ss.ps.plan->qual && chunk_state->vectorized_quals_original)
	{
		/*
		 * The normal explain won't show this if there are no normal quals but
		 * only the vectorized ones.
		 */
		ts_show_instrumentation_count("Rows Removed by Filter", 1, &node->ss.ps, es);
	}

	if (es->verbose || es->format != EXPLAIN_FORMAT_TEXT)
	{
		if (chunk_state->batch_sorted_merge)
		{
			ExplainPropertyBool("Sorted merge append", chunk_state->batch_sorted_merge, es);
		}

		if (es->analyze && (es->verbose || es->format != EXPLAIN_FORMAT_TEXT))
		{
			ExplainPropertyBool("Bulk Decompression", chunk_state->enable_bulk_decompression, es);
		}

		if (chunk_state->perform_vectorized_aggregation)
		{
			ExplainPropertyBool("Vectorized Aggregation",
								chunk_state->perform_vectorized_aggregation,
								es);
		}
	}
}
