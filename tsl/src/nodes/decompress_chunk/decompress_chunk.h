/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#ifndef TIMESCALEDB_DECOMPRESS_CHUNK_H
#define TIMESCALEDB_DECOMPRESS_CHUNK_H

#include <postgres.h>
#include <nodes/bitmapset.h>
#include <nodes/extensible.h>

#include "chunk.h"
#include "hypertable.h"

typedef struct CompressionInfo
{
	RelOptInfo *chunk_rel;
	RelOptInfo *compressed_rel;
	RangeTblEntry *chunk_rte;
	RangeTblEntry *compressed_rte;
	RangeTblEntry *ht_rte;

	int hypertable_id;
	List *hypertable_compression_info;

	int num_orderby_columns;
	int num_segmentby_columns;

	/* chunk attribute numbers that are segmentby columns */
	Bitmapset *chunk_segmentby_attnos;
	/*
	 * Chunk segmentby attribute numbers that are equated to a constant by a
	 * baserestrictinfo.
	 */
	Bitmapset *chunk_const_segmentby;
	/* compressed chunk attribute numbers for columns that are compressed */
	Bitmapset *compressed_attnos_in_compressed_chunk;

	bool single_chunk; /* query on explicit chunk */

} CompressionInfo;

typedef struct ColumnCompressionInfo
{
	FormData_hypertable_compression fd;
	bool bulk_decompression_possible;
} DecompressChunkColumnCompression;

typedef struct DecompressChunkPath
{
	CustomPath custom_path;
	CompressionInfo *info;
	/*
	 * decompression_map maps targetlist entries of the compressed scan to tuple
	 * attribute number of the uncompressed chunk. Negative values are special
	 * columns in the compressed scan that do not have a representation in the
	 * uncompressed chunk, but are still used for decompression.
	 */
	List *decompression_map;

	/*
	 * This Int list is parallel to the compressed scan targetlist, just like
	 * the above one. The value is true if a given targetlist entry is a
	 * segmentby column, false otherwise. Has the same length as the above list.
	 * We have to use the parallel lists and not a list of structs, because the
	 * Plans have to be copyable by the Postgres _copy functions, and we can't
	 * do that for a custom struct.
	 */
	List *is_segmentby_column;

	/*
	 * Same structure as above, says whether we support bulk decompression for this
	 * column.
	 */
	List *bulk_decompression_column;

	/*
	 * If we produce at least some columns that support bulk decompression.
	 */
	bool have_bulk_decompression_columns;

	/*
	 * Maps the uncompressed chunk attno to the respective column compression
	 * info. This lives only during planning so that we can understand on which
	 * columns we can apply vectorized quals.
	 */
	DecompressChunkColumnCompression *uncompressed_chunk_attno_to_compression_info;

	/*
	 * Are we able to execute a vectorized aggregation
	 */
	bool perform_vectorized_aggregation;

	/*
	 * Columns that are used for vectorized aggregates. The list contains for each attribute -1 if
	 * this is not an vectorized aggregate column or the Oid of the data type of the attribute.
	 */
	List *vectorized_aggregation_column;

	List *compressed_pathkeys;
	bool needs_sequence_num;
	bool reverse;
	bool batch_sorted_merge;
} DecompressChunkPath;

void ts_decompress_chunk_generate_paths(PlannerInfo *root, RelOptInfo *rel, Hypertable *ht,
										Chunk *chunk);

FormData_hypertable_compression *get_column_compressioninfo(List *hypertable_compression_info,
															char *column_name);

extern bool ts_is_decompress_chunk_path(Path *path);

#endif /* TIMESCALEDB_DECOMPRESS_CHUNK_H */
