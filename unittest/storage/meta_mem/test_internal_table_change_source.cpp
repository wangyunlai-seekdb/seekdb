/*
 * Copyright (c) 2025 OceanBase.
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

#include <atomic>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "share/ob_internal_table_change_notifier.h"
#include "share/ob_server_struct.h"
#include "storage/meta_mem/ob_tablet_pointer_map.h"
#include "storage/tablet/ob_tablet.h"

namespace oceanbase
{
namespace storage
{
using namespace common;
using namespace share;

namespace
{
SCN test_gts = SCN::max_scn();
int test_gts_ret = OB_SUCCESS;

int get_test_gts(SCN &gts)
{
  gts = test_gts;
  return test_gts_ret;
}
} // namespace

class TestInternalTableChangeSource : public ::testing::Test
{
protected:
  static constexpr uint64_t TABLE_A = 101;
  static constexpr uint64_t TABLE_B = 102;

  void SetUp() override
  {
    previous_notifier_ = GCTX.internal_table_change_notifier_;
    test_gts = SCN::max_scn();
    test_gts_ret = OB_SUCCESS;
    ASSERT_EQ(OB_SUCCESS, map_.init(17, ObMemAttr("TableChangeTest"), allocator_));
    ASSERT_EQ(OB_SUCCESS, notifier_.init());
    ASSERT_EQ(OB_SUCCESS, notifier_.bind_source(map_, get_test_gts));
    GCTX.internal_table_change_notifier_ = &notifier_;
  }

  void TearDown() override
  {
    GCTX.internal_table_change_notifier_ = previous_notifier_;
    notifier_.destroy();
    map_.destroy();
  }

  static SCN scn(const uint64_t offset)
  {
    return SCN::plus(SCN::base_scn(), offset);
  }

  int insert(const uint64_t table_id)
  {
    ObTabletPointer pointer;
    ObMetaDiskAddr address;
    address.set_none_addr();
    pointer.set_addr_with_reset_obj(address);
    return map_.set(ObTabletMapKey(ObTabletID(table_id)), pointer);
  }

  int erase(const uint64_t table_id)
  {
    ObMetaObjGuard<ObTablet> guard;
    return map_.erase(ObTabletMapKey(ObTabletID(table_id)), guard);
  }

  void expect_check(InternalTableChangeWatcher &watcher,
                    const InternalTableChangeWatcher::CheckResult expected)
  {
    InternalTableChangeWatcher::CheckResult actual = InternalTableChangeWatcher::NO_CHANGE;
    ASSERT_EQ(OB_SUCCESS, watcher.check_changed(actual));
    EXPECT_EQ(expected, actual);
  }

  ObArenaAllocator allocator_;
  ObTabletPointerMap map_;
  InternalTableChangeNotifier notifier_;
  InternalTableChangeNotifier *previous_notifier_ = nullptr;
};

TEST_F(TestInternalTableChangeSource, first_refresh_establishes_baseline)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  InternalTableChangeWatcher watcher;
  ASSERT_EQ(OB_SUCCESS, watcher.init_watched_tables({TABLE_A}));
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::NO_CHANGE);
}

TEST_F(TestInternalTableChangeSource, commit_during_refresh_remains_pending)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  InternalTableChangeWatcher watcher;
  ASSERT_EQ(OB_SUCCESS, watcher.init_watched_tables({TABLE_A}));
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  notifier_.notify_table_changed(TABLE_A, scn(100));
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::NO_CHANGE);
}

TEST_F(TestInternalTableChangeSource, old_and_same_scn_advance_sequence_without_lowering_watermark)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  notifier_.notify_table_changed(TABLE_A, scn(200));
  InternalTableChangeVersion previous;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, previous));
  for (const SCN commit_version : {scn(100), scn(200)}) {
    notifier_.notify_table_changed(TABLE_A, commit_version);
    InternalTableChangeVersion current;
    ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, current));
    EXPECT_GT(current.change_seq_, previous.change_seq_);
    EXPECT_EQ(scn(200), current.commit_version_);
    previous = current;
  }
}

TEST_F(TestInternalTableChangeSource, activation_during_refresh_remains_pending)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  InternalTableChangeWatcher watcher;
  ASSERT_EQ(OB_SUCCESS, watcher.init_watched_tables({TABLE_A}));
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  notifier_.mark_all_tables_changed();
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::NO_CHANGE);
}

TEST_F(TestInternalTableChangeSource, deletion_recreation_keeps_watermark_and_changes_identity)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  notifier_.notify_table_changed(TABLE_A, scn(200));
  InternalTableChangeVersion before;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, before));
  ASSERT_NE(0U, before.change_seq_);
  ASSERT_EQ(OB_SUCCESS, erase(TABLE_A));
  InternalTableChangeVersion missing;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, missing));
  EXPECT_EQ(0U, missing.change_seq_);
  EXPECT_EQ(scn(200), missing.commit_version_);
  EXPECT_GT(missing.invalidation_seq_, before.invalidation_seq_);
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  InternalTableChangeVersion recreated;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, recreated));
  EXPECT_GT(recreated.change_seq_, before.change_seq_);
  EXPECT_EQ(scn(200), recreated.commit_version_);
}

TEST_F(TestInternalTableChangeSource, publication_uses_current_entry_even_while_old_handle_is_alive)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  ObTabletPointerHandle old_handle(map_);
  ASSERT_EQ(OB_SUCCESS, map_.get(ObTabletMapKey(ObTabletID(TABLE_A)), old_handle));
  ASSERT_EQ(OB_SUCCESS, erase(TABLE_A));
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  ObTabletPointerHandle current_handle(map_);
  ASSERT_EQ(OB_SUCCESS, map_.get(ObTabletMapKey(ObTabletID(TABLE_A)), current_handle));
  ASSERT_NE(old_handle.get_resource_ptr(), current_handle.get_resource_ptr());
  InternalTableChangeVersion before;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, before));
  notifier_.notify_table_changed(TABLE_A, scn(300));
  InternalTableChangeVersion after;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, after));
  EXPECT_GT(after.change_seq_, before.change_seq_);
  EXPECT_EQ(scn(300), after.commit_version_);
}

TEST_F(TestInternalTableChangeSource, missing_pointer_refresh_never_acknowledges_an_incomplete_baseline)
{
  InternalTableChangeWatcher watcher;
  ASSERT_EQ(OB_SUCCESS, watcher.init_watched_tables({TABLE_A}));
  for (int i = 0; i < 3; ++i) {
    expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
    ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  }
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::NO_CHANGE);
}

TEST_F(TestInternalTableChangeSource, missing_publication_preserves_global_visibility_barrier)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  InternalTableChangeWatcher watcher;
  ASSERT_EQ(OB_SUCCESS, watcher.init_watched_tables({TABLE_A}));
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  notifier_.notify_table_changed(TABLE_B, scn(300));
  test_gts = scn(299);
  expect_check(watcher, InternalTableChangeWatcher::CHANGE_NOT_VISIBLE);
  EXPECT_EQ(OB_STATE_NOT_MATCH, watcher.mark_refreshed());
  test_gts = scn(300);
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::NO_CHANGE);
}

TEST_F(TestInternalTableChangeSource, missing_pointer_itself_waits_for_fallback_commit_visibility)
{
  notifier_.notify_table_changed(TABLE_A, scn(300));
  InternalTableChangeWatcher watcher;
  ASSERT_EQ(OB_SUCCESS, watcher.init_watched_tables({TABLE_A}));
  test_gts = scn(299);
  expect_check(watcher, InternalTableChangeWatcher::CHANGE_NOT_VISIBLE);
  test_gts = scn(300);
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
}

TEST_F(TestInternalTableChangeSource, mixed_watch_set_retries_until_all_pointers_are_available)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  InternalTableChangeWatcher watcher;
  ASSERT_EQ(OB_SUCCESS, watcher.init_watched_tables({TABLE_A, TABLE_B}));
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  notifier_.notify_table_changed(TABLE_A, scn(100));
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_B));
  // Even though B now exists, the captured incomplete batch is not a baseline.
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::NO_CHANGE);
}

TEST_F(TestInternalTableChangeSource, rejects_user_and_virtual_table_ids)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  InternalTableChangeVersion before;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, before));
  for (const uint64_t table_id : {OB_MIN_USER_OBJECT_ID, OB_MAX_SYS_TABLE_ID + 1}) {
    InternalTableChangeWatcher watcher;
    EXPECT_EQ(OB_INVALID_ARGUMENT, watcher.init_watched_tables({table_id}));
    InternalTableChangeVersion rejected;
    EXPECT_EQ(OB_INVALID_ARGUMENT, map_.get_change_version(table_id, rejected));
    notifier_.notify_table_changed(table_id, scn(500));
    InternalTableChangeVersion after;
    ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, after));
    EXPECT_TRUE(before == after);
    // Failed registration leaves the watcher reusable through its public API.
    ASSERT_EQ(OB_SUCCESS, watcher.init_watched_tables({TABLE_A}));
  }
}

TEST_F(TestInternalTableChangeSource, concurrent_publishers_retain_highest_commit_scn)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  InternalTableChangeVersion before;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, before));
  std::atomic<bool> start{false};
  std::vector<std::thread> publishers;
  constexpr int THREADS = 4;
  constexpr int COMMITS = 100;
  for (int worker = 0; worker < THREADS; ++worker) {
    publishers.emplace_back([this, worker, &start]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int i = COMMITS; i > 0; --i) {
        notifier_.notify_table_changed(TABLE_A, scn(worker * COMMITS + i));
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (std::thread &publisher : publishers) {
    publisher.join();
  }
  InternalTableChangeVersion after;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, after));
  EXPECT_EQ(before.change_seq_ + THREADS * COMMITS, after.change_seq_);
  EXPECT_EQ(scn(THREADS * COMMITS), after.commit_version_);
}

TEST_F(TestInternalTableChangeSource, object_reset_and_address_replacement_preserve_change_marker)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  notifier_.notify_table_changed(TABLE_A, scn(200));
  InternalTableChangeVersion before;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, before));
  ObTabletPointerHandle handle(map_);
  ASSERT_EQ(OB_SUCCESS, map_.get(ObTabletMapKey(ObTabletID(TABLE_A)), handle));
  ASSERT_NE(nullptr, handle.get_resource_ptr());
  handle.get_resource_ptr()->reset_obj();
  InternalTableChangeVersion after_reset;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, after_reset));
  EXPECT_TRUE(before == after_reset);
  ObMetaDiskAddr address;
  ASSERT_EQ(OB_SUCCESS, address.set_local_addr(TABLE_A, 4096, 0));
  handle.get_resource_ptr()->set_addr_with_reset_obj(address);
  InternalTableChangeVersion after_address;
  ASSERT_EQ(OB_SUCCESS, map_.get_change_version(TABLE_A, after_address));
  EXPECT_TRUE(before == after_address);
  EXPECT_TRUE(address == handle.get_resource_ptr()->get_addr());
}

TEST_F(TestInternalTableChangeSource, gts_failure_does_not_acknowledge_a_pending_refresh)
{
  ASSERT_EQ(OB_SUCCESS, insert(TABLE_A));
  InternalTableChangeWatcher watcher;
  ASSERT_EQ(OB_SUCCESS, watcher.init_watched_tables({TABLE_A}));
  test_gts_ret = OB_EAGAIN;
  InternalTableChangeWatcher::CheckResult result = InternalTableChangeWatcher::NO_CHANGE;
  EXPECT_EQ(OB_EAGAIN, watcher.check_changed(result));
  EXPECT_EQ(OB_STATE_NOT_MATCH, watcher.mark_refreshed());
  test_gts_ret = OB_SUCCESS;
  expect_check(watcher, InternalTableChangeWatcher::REFRESH_ALLOWED);
  ASSERT_EQ(OB_SUCCESS, watcher.mark_refreshed());
  expect_check(watcher, InternalTableChangeWatcher::NO_CHANGE);
}

} // namespace storage
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
