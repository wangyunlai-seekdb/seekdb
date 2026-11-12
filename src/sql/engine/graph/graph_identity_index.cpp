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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/graph/graph_identity_index.h"
#include "common/wide_integer/ob_wide_integer.h"
#include "sql/engine/expr/ob_expr_util.h"
#include "share/ob_errno.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
GraphIdentityIndex::GraphIdentityIndex(ObIAllocator &allocator, Ownership ownership)
    : bucket_heads_(ModulePageAllocator(allocator, "GraphIdIndex")),
      entries_(ModulePageAllocator(allocator, "GraphIdIndex")),
      membership_epochs_(ModulePageAllocator(allocator, "GraphIdIndex")),
      identity_allocator_(allocator), ownership_(ownership)
{
}

int GraphIdentityIndex::init(int64_t expected_count)
{
  int ret = OB_SUCCESS;
  reset();
  if (OB_UNLIKELY(expected_count < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid graph identity count", K(ret), K(expected_count));
  } else {
    const int64_t bucket_count = std::max<int64_t>(1, expected_count);
    if (OB_FAIL(bucket_heads_.prepare_allocate(bucket_count))) {
      LOG_WARN("failed to allocate graph identity buckets", K(ret),
               K(bucket_count));
    } else {
      for (int64_t i = 0; i < bucket_heads_.count(); ++i) {
        bucket_heads_.at(i) = -1;
      }
    }
  }
  return ret;
}

int GraphIdentityIndex::calc_compatible_hash(
    const GraphElementIdentity &identity,
    uint64_t &hash) const
{
  int ret = OB_SUCCESS;
  hash = do_hash(identity.graph_id_, 0);
  hash = do_hash(identity.element_id_, hash);
  for (int64_t i = 0;
       OB_SUCC(ret) && i < identity.rowkey_.get_obj_cnt(); ++i) {
    const ObObj &key = identity.rowkey_.get_obj_ptr()[i];
    if (key.is_decimal_int()) {
      // Graph endpoint compatibility follows ordinary key comparison and
      // permits DECIMAL_INT <-> NUMBER as well as different decimal scales.
      // ObObj's raw hash includes the concrete type and decimal scale, so
      // normalize this numeric domain before choosing a hash bucket. The
      // index still uses the full typed identity for collision equality.
      ObNumStackOnceAlloc number_allocator;
      number::ObNumber number;
      ObObj normalized;
      if (OB_FAIL(wide::to_number(key.get_decimal_int(), key.get_int_bytes(),
                                  key.get_scale(), number_allocator, number))) {
        LOG_WARN("failed to normalize graph decimal identity", K(ret), K(key));
      } else {
        normalized.set_number(ObNumberType, number);
        ret = normalized.hash(hash, hash);
      }
    } else if (key.get_type_class() == ObNumberTC) {
      // Signed and unsigned NUMBER values share comparison semantics for
      // non-negative values. Hash both with the same canonical type so a
      // legal NUMBER <-> DECIMAL_INT endpoint mapping reaches one bucket.
      ObObj normalized;
      normalized.set_number(ObNumberType, key.get_number());
      ret = normalized.hash(hash, hash);
    } else if (key.get_type_class() == ObDoubleTC) {
      // Fixed-scale DOUBLE comparison is tolerance based and can compare
      // equal across different declared scales. No value-dependent hash can
      // preserve that non-exact equality for every boundary value, so keep
      // DOUBLE keys in a type bucket and let typed equality resolve collisions.
      hash = do_hash(key.get_type(), hash);
    } else {
      ret = key.hash(hash, hash);
    }
  }
  return ret;
}

int GraphIdentityIndex::get_hash(
    const GraphElementIdentity &identity,
    uint64_t &hash,
    int64_t binding_id) const
{
  int ret = OB_SUCCESS;
  hash = 0;
  if (bucket_heads_.empty()) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(!identity.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(calc_compatible_hash(identity, hash))) {
    LOG_WARN("failed to hash graph identity", K(ret), K(identity));
  } else {
    hash = do_hash(binding_id, hash);
  }
  return ret;
}

int GraphIdentityIndex::find(const GraphElementIdentity &identity,
                             int64_t &entry_index,
                             bool &found,
                             int64_t binding_id) const
{
  int ret = OB_SUCCESS;
  uint64_t hash = 0;
  entry_index = -1;
  found = false;
  if (OB_FAIL(get_hash(identity, hash, binding_id))) {
    LOG_WARN("failed to locate graph identity bucket", K(ret), K(identity));
  } else {
    ret = find_in_bucket(identity, hash, binding_id, entry_index, found);
  }
  return ret;
}

int GraphIdentityIndex::find_in_bucket(const GraphElementIdentity &identity,
                                      uint64_t hash, int64_t binding_id,
                                      int64_t &entry_index, bool &found) const
{
  int ret = OB_SUCCESS;
  int64_t visited = 0;
  entry_index = -1;
  found = false;
  if (OB_UNLIKELY(bucket_heads_.empty())) {
    ret = OB_NOT_INIT;
  } else {
    const int64_t bucket = hash % bucket_heads_.count();
    int64_t current = bucket_heads_.at(bucket);
    while (OB_SUCC(ret) && !found && current >= 0) {
      if (OB_UNLIKELY(current >= entries_.count()
                      || visited++ >= entries_.count())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("corrupted graph identity hash chain", K(ret), K(current),
                 K(visited), "entry_count", entries_.count());
      } else if ((ownership_ == Ownership::BORROWED
                  || membership_epochs_.at(current) == current_epoch_)
                 && entries_.at(current).hash_ == hash
                 && entries_.at(current).binding_id_ == binding_id
                 && entries_.at(current).identity_ == identity) {
        entry_index = current;
        found = true;
      } else {
        current = entries_.at(current).next_entry_;
      }
    }
  }
  return ret;
}

int GraphIdentityIndex::get_or_insert(
    const GraphElementIdentity &identity,
    int64_t &entry_index,
    bool &inserted,
    int64_t binding_id)
{
  int ret = OB_SUCCESS;
  bool found = false;
  uint64_t hash = 0;
  entry_index = -1;
  inserted = false;
  if (OB_UNLIKELY(ownership_ != Ownership::BORROWED)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(get_hash(identity, hash, binding_id))) {
  } else if (OB_FAIL(find_in_bucket(identity, hash, binding_id, entry_index, found))) {
  } else if (found) {
  } else if (OB_FAIL(grow_buckets())) {
  } else {
    const int64_t bucket = hash % bucket_heads_.count();
    Entry entry;
    entry.identity_ = identity;
    entry.binding_id_ = binding_id;
    entry.hash_ = hash;
    entry.next_entry_ = bucket_heads_.at(bucket);
    if (OB_FAIL(entries_.push_back(entry))) {
      LOG_WARN("failed to append graph identity entry", K(ret), K(identity));
    } else {
      entry_index = entries_.count() - 1;
      bucket_heads_.at(bucket) = entry_index;
      inserted = true;
    }
  }
  return ret;
}

int GraphIdentityIndex::grow_buckets()
{
  int ret = OB_SUCCESS;
  // Reachable frontiers can grow far beyond their input level's size. Keep a
  // bounded load factor instead of degrading to linear chains at a high-degree
  // vertex. Entry positions (used by existing adjacency callers) stay stable.
  if (entries_.count() >= bucket_heads_.count()) {
    if (bucket_heads_.count() > INT64_MAX / 2) {
      ret = OB_SIZE_OVERFLOW;
    } else if (OB_FAIL(bucket_heads_.prepare_allocate(bucket_heads_.count() * 2))) {
    } else {
      for (int64_t i = 0; i < bucket_heads_.count(); ++i) {
        bucket_heads_.at(i) = -1;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < entries_.count(); ++i) {
        Entry &entry = entries_.at(i);
        // Identity backing is immutable while indexed. Rehash only the saved
        // compatible hash; do not renormalize decimal/string keys on growth.
        const int64_t bucket = entry.hash_ % bucket_heads_.count();
        entry.next_entry_ = bucket_heads_.at(bucket);
        bucket_heads_.at(bucket) = i;
      }
    }
  }
  return ret;
}

void GraphIdentityIndex::reset()
{
  bucket_heads_.reset();
  entries_.reset();
  membership_epochs_.reset();
  identity_allocator_.reset();
  current_epoch_ = 1;
}

int64_t GraphIdentityIndex::used_memory() const
{
  int64_t used = 0;
  const int64_t parts[] = {bucket_heads_.mem_used(), entries_.mem_used(),
      membership_epochs_.mem_used(), identity_allocator_.total()};
  for (int64_t part : parts) {
    if (part < 0 || used > INT64_MAX - part) { return INT64_MAX; }
    used += part;
  }
  return used;
}

int GraphIdentityIndex::retain_identity(const GraphElementIdentity &identity,
                                        int64_t binding_id, int64_t &handle)
{
  int ret = OB_SUCCESS;
  uint64_t hash = 0;
  Entry entry;
  handle = -1;
  if (OB_UNLIKELY(ownership_ != Ownership::OWNED_ENDPOINTS || binding_id < 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (bucket_heads_.empty() && OB_FAIL(init(1))) {
  } else if (OB_FAIL(get_hash(identity, hash, binding_id))) {
  } else if (OB_FAIL(grow_buckets())) {
  } else if (OB_FAIL(identity.rowkey_.deep_copy(entry.identity_.rowkey_, identity_allocator_))) {
  } else {
    entry.identity_.graph_id_ = identity.graph_id_;
    entry.identity_.element_id_ = identity.element_id_;
    entry.binding_id_ = binding_id;
    entry.hash_ = hash;
    const int64_t bucket = hash % bucket_heads_.count();
    entry.next_entry_ = bucket_heads_.at(bucket);
    if (OB_FAIL(entries_.push_back(entry))) {
    } else if (OB_FAIL(membership_epochs_.push_back(0))) {
    } else {
      handle = entries_.count() - 1;
      bucket_heads_.at(bucket) = handle;
    }
  }
  // Allocation errors are terminal to the owner; never leave a partial entry
  // whose membership byte could be read by a subsequent probe.
  if (OB_FAIL(ret)) { reset(); }
  return ret;
}

int GraphIdentityIndex::get_identity(int64_t handle, GraphElementIdentity &identity,
                                     int64_t &binding_id) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ownership_ != Ownership::OWNED_ENDPOINTS
                  || handle < 0 || handle >= entries_.count())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    identity = entries_.at(handle).identity_;
    binding_id = entries_.at(handle).binding_id_;
  }
  return ret;
}

int GraphIdentityIndex::mark_present(int64_t handle)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ownership_ != Ownership::OWNED_ENDPOINTS
                  || handle < 0 || handle >= membership_epochs_.count())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    membership_epochs_.at(handle) = current_epoch_;
  }
  return ret;
}

int GraphIdentityIndex::clear_membership()
{
  static_assert(2 * GRAPH_PATH_MAX_HOPS + 2 < UINT8_MAX,
                "one membership epoch per bounded BFS transition must fit");
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ownership_ != Ownership::OWNED_ENDPOINTS)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(current_epoch_ == UINT8_MAX)) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    ++current_epoch_;
  }
  return ret;
}


} // namespace sql
} // namespace oceanbase
