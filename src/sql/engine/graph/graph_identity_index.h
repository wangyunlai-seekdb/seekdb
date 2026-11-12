/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "lib/container/ob_array.h"
#include "lib/container/ob_2d_array.h"
#include "lib/allocator/page_arena.h"
#include "sql/engine/graph/graph_path_spec.h"

namespace oceanbase
{
namespace sql
{
// Query-local hash index over typed graph identities. Compatible NUMBER and
// DECIMAL_INT keys are normalized, while tolerance-compared DOUBLE keys use a
// type bucket, so bucket selection follows graph endpoint equality. Identities
// are shallow views in BORROWED mode: the caller keeps rowkey backing alive
// until reset(). OWNED_ENDPOINTS retains exact accepted identities and exposes
// only currently marked membership to find(). An optional binding ID separates
// correlation domains without changing element identity. All allocations use
// the owner's graph work area.
class GraphIdentityIndex final
{
public:
  enum class Ownership : uint8_t { BORROWED, OWNED_ENDPOINTS };
  explicit GraphIdentityIndex(common::ObIAllocator &allocator,
                              Ownership ownership = Ownership::BORROWED);
  ~GraphIdentityIndex() = default;

  int init(int64_t expected_count);
  // Borrowed mode insertion. The caller owns immutable rowkey backing.
  int get_or_insert(const GraphElementIdentity &identity,
                    int64_t &entry_index,
                    bool &inserted,
                    int64_t binding_id = 0);
  int find(const GraphElementIdentity &identity,
           int64_t &entry_index,
           bool &found,
           int64_t binding_id = 0) const;
  // Owned-only: retain the exact occurrence, initially absent from find().
  // Never replace an identity with a tolerance-equal DOUBLE representative.
  int retain_identity(const GraphElementIdentity &identity, int64_t binding_id,
                       int64_t &handle);
  int get_identity(int64_t handle, GraphElementIdentity &identity,
                    int64_t &binding_id) const;
  int mark_present(int64_t handle);
  // Clears eligible membership without invalidating owned identities/handles.
  int clear_membership();
  void reset();
  int64_t count() const { return entries_.count(); }
  static int64_t entry_size() { return sizeof(Entry); }
  int64_t used_memory() const;

private:
  struct Entry
  {
    GraphElementIdentity identity_{};
    // Optional correlation domain; adjacency-only users keep the default zero.
    int64_t binding_id_{0};
    int64_t next_entry_{-1};
    uint64_t hash_{0};
    TO_STRING_KV(K_(identity), K_(next_entry));
  };

  int calc_compatible_hash(const GraphElementIdentity &identity,
                           uint64_t &hash) const;
  int get_hash(const GraphElementIdentity &identity,
               uint64_t &hash, int64_t binding_id) const;
  int find_in_bucket(const GraphElementIdentity &identity, uint64_t hash,
                     int64_t binding_id, int64_t &entry_index, bool &found) const;
  int grow_buckets();

private:
  // Rehash changes logical bucket positions, not their physical storage.
  // Growing appends bounded blocks instead of duplicating a large buffer.
  common::ObSegmentArray<int64_t, common::OB_MALLOC_NORMAL_BLOCK_SIZE> bucket_heads_;
  // Logical positions and hash-chain links stay stable without copying all
  // entries on growth. Normal-size segments also bound unused tail capacity.
  // The shared allocator charges the segment directory as well as the blocks.
  common::ObSegmentArray<Entry, common::OB_MALLOC_NORMAL_BLOCK_SIZE> entries_;
  // Only the endpoint owner populates these. Borrowing index users retain the
  // same Entry layout and allocate neither key copies nor membership bytes.
  common::ObSegmentArray<uint8_t, common::OB_MALLOC_NORMAL_BLOCK_SIZE> membership_epochs_;
  common::ObArenaAllocator identity_allocator_;
  Ownership ownership_{Ownership::BORROWED};
  uint8_t current_epoch_{1};

  DISALLOW_COPY_AND_ASSIGN(GraphIdentityIndex);
};


} // namespace sql
} // namespace oceanbase
