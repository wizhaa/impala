// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef IMPALA_EXEC_PARQUET_SCRATCH_TUPLE_BATCH_H
#define IMPALA_EXEC_PARQUET_SCRATCH_TUPLE_BATCH_H

#include "runtime/descriptors.h"
#include "runtime/row-batch.h"
#include "runtime/tuple-row.h"

namespace impala {

/// Helper struct that holds a batch of tuples allocated from a mem pool, as well
/// as state associated with iterating over its tuples and transferring
/// them to an output batch in TransferScratchTuples().
struct ScratchTupleBatch {
  // Memory for the fixed-length parts of the batch of tuples. Allocated from
  // 'tuple_mem_pool'. Set to NULL when transferred to an output batch.
  uint8_t* tuple_mem = nullptr;
  // Number of tuples that can be stored in 'tuple_mem'.
  int capacity;
  // Keeps track of the current tuple index.
  int tuple_idx = 0;
  // Number of valid tuples in tuple_mem.
  int num_tuples = 0;
  // Number of tuples transferred to the output batch (i.e. not filtered by predicates).
  int num_tuples_transferred = 0;
  // Number of different output batches tuples were transferred to.
  int num_non_empty_output_batches = 0;
  // Bytes of fixed-length data per tuple.
  const int tuple_byte_size;

  // Pool used to allocate 'tuple_mem' and nothing else.
  MemPool tuple_mem_pool;

  // Pool used to accumulate other memory that may be referenced by var-len slots in this
  // batch, e.g. decompression buffers, allocations for var-len strings and allocations
  // for nested arrays. This memory may be referenced by previous batches or the current
  // batch, but not by future batches. E.g. a decompression buffer can be safely attached
  // only once all values referencing that buffer have been materialized into the batch.
  MemPool aux_mem_pool;

  // Tuples transferred to an output row batch are compacted if
  // (# tuples materialized / # tuples returned) exceeds this number. Chosen so that the
  // cost of copying the tuples should be very small in relation to the original cost of
  // materialising them.
  const int MIN_SELECTIVITY_TO_COMPACT = 16;

  ScratchTupleBatch(
      const RowDescriptor& row_desc, int batch_size, MemTracker* mem_tracker)
    : capacity(batch_size),
      tuple_byte_size(row_desc.GetRowSize()),
      tuple_mem_pool(mem_tracker),
      aux_mem_pool(mem_tracker) {
    DCHECK_EQ(row_desc.tuple_descriptors().size(), 1);
  }

  Status Reset(RuntimeState* state) {
    tuple_idx = 0;
    num_tuples = 0;
    num_tuples_transferred = 0;
    num_non_empty_output_batches = 0;
    if (tuple_mem == nullptr) {
      int64_t dummy;
      RETURN_IF_ERROR(RowBatch::ResizeAndAllocateTupleBuffer(
          state, &tuple_mem_pool, tuple_byte_size, &capacity, &dummy, &tuple_mem));
    }
    return Status::OK();
  }

  /// Release all memory in the MemPools. If 'dst_pool' is non-NULL, transfers it to
  /// 'dst_pool'. Otherwise frees the memory.
  void ReleaseResources(MemPool* dst_pool) {
    if (dst_pool == nullptr) {
      tuple_mem_pool.FreeAll();
      aux_mem_pool.FreeAll();
    } else {
      dst_pool->AcquireData(&tuple_mem_pool, false);
      dst_pool->AcquireData(&aux_mem_pool, false);
    }
    tuple_mem = nullptr;
  }

  /// Finalize transfer of 'num_to_commit' tuples to 'dst_batch' and transfer memory to
  /// 'dst_batch' if at the end of 'scratch_batch'. The tuples must not yet be
  /// committed to 'dst_batch'. Only needs to be called when materialising non-empty
  /// tuples.
  void FinalizeTupleTransfer(RowBatch* dst_batch, int num_to_commit) {
    DCHECK_LE(dst_batch->num_rows() + num_to_commit, dst_batch->capacity());
    num_non_empty_output_batches += (num_to_commit > 0);
    num_tuples_transferred += num_to_commit;
    if (!AtEnd()) return;
    // We're at the end of the scratch batch. Transfer memory that may be referenced by
    // transferred tuples or that we can't reuse to 'dst_batch'.

    // Future tuples won't reference data in 'aux_mem_pool' - always transfer so that
    // we don't accumulate unneeded memory in the scratch batch.
    dst_batch->tuple_data_pool()->AcquireData(&aux_mem_pool, false);

    // Try to avoid the transfer of 'tuple_mem' for selective scans by compacting the
    // output batch. This avoids excessive allocation and transfer of memory, which
    // can lead to performance problems like IMPALA-4923.
    // Compaction is unsafe if the scratch batch was split across multiple output batches
    // because the batch we returned earlier may hold a reference into 'tuple_mem'.
    if (num_tuples_transferred * MIN_SELECTIVITY_TO_COMPACT > capacity
        || num_non_empty_output_batches > 1
        || !TryCompact(dst_batch, num_to_commit)) {
      // Didn't compact - rows in 'dst_batch' reference 'tuple_mem'.
      dst_batch->tuple_data_pool()->AcquireData(&tuple_mem_pool, false);
      tuple_mem = nullptr;
    }
  }

  /// Try to compact 'num_uncommitted_tuples' uncommitted tuples that were added to
  /// the end of 'dst_batch' by copying them to memory allocated from
  /// dst_batch->tuple_data_pool(). Returns true on success or false if the memory
  /// could not be allocated.
  bool TryCompact(RowBatch* dst_batch, int num_uncommitted_tuples) {
    DCHECK_LE(dst_batch->num_rows() + num_uncommitted_tuples, dst_batch->capacity());

    // Copy rows that reference 'tuple_mem' into a new small buffer. This code handles
    // the case where num_uncommitted_tuples == 0, since TryAllocate() returns a non-null
    // pointer.
    int64_t dst_bytes = num_uncommitted_tuples * static_cast<int64_t>(tuple_byte_size);
    uint8_t* dst_buffer = dst_batch->tuple_data_pool()->TryAllocate(dst_bytes);
    if (dst_buffer == nullptr) return false;
    const int end_row = dst_batch->num_rows() + num_uncommitted_tuples;
    for (int i = dst_batch->num_rows(); i < end_row; ++i) {
      TupleRow* row = dst_batch->GetRow(i);
      Tuple* uncompacted_tuple = row->GetTuple(0);
      DCHECK_GE(reinterpret_cast<uint8_t*>(uncompacted_tuple), tuple_mem);
      DCHECK_LT(reinterpret_cast<uint8_t*>(uncompacted_tuple),
          tuple_mem + tuple_byte_size * capacity);
      row->SetTuple(0, reinterpret_cast<Tuple*>(dst_buffer));
      memcpy(dst_buffer, uncompacted_tuple, tuple_byte_size);
      dst_buffer += tuple_byte_size;
    }
    return true;
  }

  inline Tuple* GetTuple(int tuple_idx) const {
    return reinterpret_cast<Tuple*>(tuple_mem + tuple_idx * tuple_byte_size);
  }

  inline uint8_t* CurrTuple() const { return tuple_mem + tuple_idx * tuple_byte_size; }
  inline uint8_t* TupleEnd() const { return tuple_mem + num_tuples * tuple_byte_size; }
  inline bool AtEnd() const { return tuple_idx == num_tuples; }
  int64_t total_allocated_bytes() const {
    return tuple_mem_pool.total_allocated_bytes() + aux_mem_pool.total_allocated_bytes();
  }
};
}

#endif
