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

#define USING_LOG_PREFIX SHARE
#include "share/ob_internal_table_change_notifier.h"
#include "lib/atomic/ob_atomic.h"
#include "lib/oblog/ob_log_module.h"
#include "share/ob_server_struct.h"

namespace oceanbase
{
namespace share
{

InternalTableChangeNotifier::InternalTableChangeNotifier() = default;

InternalTableChangeNotifier::~InternalTableChangeNotifier()
{
  destroy();
}

int InternalTableChangeNotifier::init()
{
  int ret = OB_SUCCESS;
  if (ATOMIC_LOAD(&is_inited_)) {
  } else {
    ATOMIC_STORE(&is_inited_, true);
    LOG_INFO("InternalTableChangeNotifier inited");
  }
  return ret;
}

void InternalTableChangeNotifier::destroy()
{
  ATOMIC_STORE(&is_inited_, false);
  source_ = nullptr;
  gts_provider_ = nullptr;
  LOG_INFO("InternalTableChangeNotifier destroyed");
}

int InternalTableChangeNotifier::bind_source(
    IInternalTableChangeSource &source, const GtsProvider provider)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(provider)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!ATOMIC_LOAD(&is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (nullptr != source_) {
    ret = OB_INIT_TWICE;
  } else {
    source_ = &source;
    gts_provider_ = provider;
  }
  return ret;
}

void InternalTableChangeNotifier::notify_table_changed(
    const uint64_t table_id, const SCN &commit_version)
{
  if (ATOMIC_LOAD(&is_inited_) && nullptr != source_) {
    source_->notify_table_changed(table_id, commit_version);
  }
}

int InternalTableChangeNotifier::get_change_version(
    const uint64_t table_id, ChangeVersion &version) const
{
  int ret = OB_SUCCESS;
  if (!ATOMIC_LOAD(&is_inited_) || nullptr == source_) {
    ret = OB_NOT_INIT;
  } else {
    ret = source_->get_change_version(table_id, version);
  }
  return ret;
}

int InternalTableChangeNotifier::is_change_visible(
    const SCN &commit_version,
    bool &is_visible) const
{
  int ret = OB_SUCCESS;
  is_visible = false;
  SCN gts;
  if (OB_UNLIKELY(!commit_version.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid commit version for visibility check", K(ret), K(commit_version));
  } else if (OB_ISNULL(gts_provider_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("GTS provider is not initialized for internal table refresh", K(ret));
  } else if (OB_FAIL(gts_provider_(gts))) {
    LOG_WARN("failed to get GTS for internal table refresh", K(ret));
  } else {
    is_visible = is_commit_version_visible(commit_version, gts);
  }
  return ret;
}

void InternalTableChangeNotifier::mark_all_tables_changed()
{
  if (ATOMIC_LOAD(&is_inited_) && nullptr != source_) {
    source_->mark_all_tables_changed();
  }
}

int InternalTableChangeWatcher::init_watched_tables(
    const std::initializer_list<uint64_t> table_ids)
{
  int ret = OB_SUCCESS;
  OB_ASSERT(watched_tables_.count() == 0);
  for (const uint64_t table_id : table_ids) {
    if (OB_UNLIKELY(!common::is_sys_table(table_id))) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("only physical system tables can be watched", K(ret), K(table_id));
    } else {
      WatchedTable watched_table;
      watched_table.table_id_ = table_id;
      if (OB_FAIL(watched_tables_.push_back(watched_table))) {
        LOG_WARN("failed to save watched table", K(ret), K(table_id));
      }
    }
    if (OB_FAIL(ret)) {
      break;
    }
  }
  if (OB_FAIL(ret)) {
    watched_tables_.reset();
    target_captured_ = false;
  }
  return ret;
}

int InternalTableChangeWatcher::check_changed(CheckResult &result)
{
  int ret = OB_SUCCESS;
  bool changed = false;
  bool is_visible = false;
  result = NO_CHANGE;
  InternalTableChangeNotifier *notifier = GCTX.internal_table_change_notifier_;
  OB_ASSERT(nullptr != notifier);
  OB_ASSERT(watched_tables_.count() > 0);
  target_captured_ = false;
  target_commit_version_ = SCN::min_scn();
  for (int64_t i = 0; OB_SUCC(ret) && i < watched_tables_.count(); ++i) {
    WatchedTable &watched_table = watched_tables_.at(i);
    if (OB_FAIL(notifier->get_change_version(
            watched_table.table_id_, watched_table.target_version_))) {
      LOG_WARN("failed to get watched internal table version", K(ret),
               "table_id", watched_table.table_id_);
    } else {
      changed = changed || 0 == watched_table.target_version_.change_seq_
                || watched_table.target_version_ !=
                               watched_table.processed_version_;
      target_commit_version_ =
          SCN::max(target_commit_version_,
                   watched_table.target_version_.commit_version_);
    }
  }
  if (OB_SUCC(ret) && !changed) {
    result = NO_CHANGE;
  } else if (OB_SUCC(ret) && OB_FAIL(notifier->is_change_visible(
                                 target_commit_version_, is_visible))) {
    LOG_WARN("failed to check watched internal table visibility", K(ret),
             K(target_commit_version_));
  } else if (OB_SUCC(ret) && !is_visible) {
    result = CHANGE_NOT_VISIBLE;
  } else if (OB_SUCC(ret)) {
    result = REFRESH_ALLOWED;
    target_captured_ = true;
  }
  return ret;
}

int InternalTableChangeWatcher::mark_refreshed()
{
  int ret = OB_SUCCESS;
  if (watched_tables_.count() <= 0) {
    ret = OB_NOT_INIT;
    LOG_WARN("internal table watcher is not registered", K(ret));
  } else if (!target_captured_) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("no refreshable target captured by internal table watcher",
             K(ret));
  } else {
    bool complete = true;
    for (int64_t i = 0; complete && i < watched_tables_.count(); ++i) {
      complete = 0 != watched_tables_.at(i).target_version_.change_seq_;
    }
    // A missing pointer keeps the capture unacknowledged so a successful
    // fallback read cannot suppress later refresh attempts.
    for (int64_t i = 0; complete && i < watched_tables_.count(); ++i) {
      WatchedTable &watched_table = watched_tables_.at(i);
      watched_table.processed_version_ = watched_table.target_version_;
    }
    target_captured_ = false;
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
