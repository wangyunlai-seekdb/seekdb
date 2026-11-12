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

#include "lib/allocator/page_arena.h"
#include "lib/container/ob_2d_array.h"
#include "lib/container/ob_array.h"
#include "sql/engine/graph/graph_path_spec.h"
#include "sql/engine/graph/graph_identity_index.h"

namespace oceanbase
{
namespace sql
{

// Frontier positions are random-access logical IDs, not a contiguous SQL row.
using GraphFrontierStateIds =
    common::ObSegmentArray<int64_t, common::OB_MALLOC_NORMAL_BLOCK_SIZE>;

// Query-local owner of parent-linked graph path prefixes. State IDs are array
// positions allocated monotonically by this store; they are not persistent
// graph IDs and remain valid only until reset(). Variable-length identity keys
// are copied into the store arena so neither frontier turnover nor DAS page
// reuse can invalidate a saved path.
class GraphPathStateStore final
{
public:
  enum class Retention : uint8_t { HISTORY, ENDPOINTS, INDEXED_ENDPOINTS };
  GraphPathStateStore(common::ObIAllocator &allocator, int64_t memory_limit);
  ~GraphPathStateStore() = default;

  // Creates the zero-hop state for one seed/outer binding. Duplicate seeds are
  // deliberately assigned different states to preserve bag multiplicity. The
  // first root fixes retention until reset(); later roots must use that policy.
  int add_root(int64_t binding_id,
               const GraphElementIdentity &source_identity,
               int64_t &path_state_id,
               Retention retention = Retention::HISTORY);

  // GraphExpandOutput::path_state_id_ names the parent prefix. This method
  // validates that association, inherits its binding and creates the child.
  int add_child(const GraphExpandOutput &extension, int64_t &path_state_id);
  // The indexed representation shares its identity descriptor with visited
  // membership. Mark an accepted occurrence, not an equality representative.
  int mark_endpoint_seen(int64_t path_state_id);
  GraphIdentityIndex &endpoint_index() { return endpoint_index_; }
  const GraphIdentityIndex &endpoint_index() const { return endpoint_index_; }
  // After a complete endpoint-only shortest BFS level, the previous level is
  // no longer needed as a parent. Keep the roots and the new frontier in
  // place, rewriting its transient state IDs before the next expansion.
  // Call only after all input/edge cursors for the old level are released.
  int compact_endpoint_frontier(GraphFrontierStateIds &frontier_state_ids);
  int build_endpoints(int64_t leaf_path_state_id,
                       common::ObIArray<GraphPathState> &endpoints) const;

  // Returned identities are shallow copies backed by this store's arena and
  // therefore remain valid until reset(). The copy itself is stable across a
  // later storage growth, unlike a pointer to an element inside states_.
  int get_state(int64_t path_state_id, GraphPathState &state) const;
  int make_expand_input(int64_t path_state_id, GraphExpandInput &input) const;

  // Reconstructs one root-to-leaf path. Returned identities are shallow views
  // backed by this store and remain valid until reset().
  int build_path(int64_t leaf_path_state_id,
                 common::ObIArray<GraphPathState> &path) const;

  // Checks only the ancestry of one path. Equal edges used by different paths
  // do not conflict, which preserves TRAIL path multiplicity.
  int contains_edge(int64_t path_state_id,
                    const GraphElementIdentity &edge_identity,
                    bool &contains) const;

  void reset();
  int64_t count() const
  { return retention_ == Retention::HISTORY ? states_.count()
      : retention_ == Retention::ENDPOINTS ? endpoints_.count() : indexed_endpoints_.count(); }
  int64_t root_count() const { return root_state_count_; }
  bool empty() const { return count() == 0; }
  int64_t used_memory() const;
  int64_t peak_memory() const { return peak_memory_bytes_; }

private:
  // Self ID is the array position. Endpoint consumers need neither ancestry
  // nor edge identity/payload; materialize the public GraphPathState on read.
  struct EndpointState
  {
    int64_t binding_id_{0};
    int64_t root_path_state_id_{GRAPH_INVALID_PATH_STATE_ID};
    int64_t hop_{0};
    GraphElementIdentity current_identity_{};
    TO_STRING_KV(K_(binding_id), K_(root_path_state_id), K_(hop), K_(current_identity));
  };
  struct IndexedEndpointState
  {
    int64_t root_path_state_id_{GRAPH_INVALID_PATH_STATE_ID};
    int64_t hop_{0};
    int64_t identity_handle_{-1};
    TO_STRING_KV(K_(root_path_state_id), K_(hop), K_(identity_handle));
  };
  int deep_copy_identity(const GraphElementIdentity &source,
                         GraphElementIdentity &destination);
  int retain_payload(const GraphEdgePayload &source, GraphEdgePayload &destination);
  int append_state(GraphPathState &state, int64_t &path_state_id);
  int check_memory_limit();
  bool is_valid_state_id(int64_t path_state_id) const;
  int fail(int error);

private:
  common::ObArenaAllocator identity_allocator_;
  int64_t memory_limit_;
  // Keep state IDs as logical indexes without requiring one contiguous buffer
  // to be reallocated and copied as the frontier grows.
  common::ObSegmentArray<GraphPathState> states_;
  common::ObSegmentArray<EndpointState> endpoints_;
  common::ObSegmentArray<IndexedEndpointState> indexed_endpoints_;
  GraphIdentityIndex endpoint_index_;
  // The first root selects storage before any records are allocated. Only
  // one array is populated until reset(); children cannot change the policy.
  Retention retention_{Retention::HISTORY};
  int64_t root_state_count_{0};
  int64_t peak_memory_bytes_{0};

  DISALLOW_COPY_AND_ASSIGN(GraphPathStateStore);
};

} // namespace sql
} // namespace oceanbase
