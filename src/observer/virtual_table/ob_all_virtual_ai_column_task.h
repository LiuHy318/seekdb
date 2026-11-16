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

#ifndef OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_VIRTUAL_AI_COLUMN_TASK_H_
#define OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_VIRTUAL_AI_COLUMN_TASK_H_

#include "observer/virtual_table/ob_virtual_table_iterator.h"
#include "share/ai_column/ob_ai_column_task_registry.h"
#include "sql/session/ob_sql_session_info.h"
#include "lib/container/ob_array.h"

namespace oceanbase
{
namespace observer
{
// oceanbase.__all_ai_column_task
//
// The AI column task queue is served from an in-memory registry instead of
// persistent rows.  A row is visible to a session when
//   * it was produced by the very transaction the session is running (writer
//     sees its own uncommitted invalidation), or
//   * the producing transaction has been observed as committed.
// Therefore a rollback makes the rows vanish, and an in-flight transaction
// never leaks rows into other sessions.
class ObAllVirtualAiColumnTask : public common::ObVirtualTableIterator
{
public:
  enum ObAiColumnTaskColumnId
  {
    COL_TENANT_ID = 0,
    COL_TASK_ID,
    COL_TABLE_ID,
    COL_COLUMN_ID,
    COL_DATABASE_NAME,
    COL_TABLE_NAME,
    COL_COLUMN_NAME,
    COL_ROW_KEY,
    COL_ROWKEY_PREDICATE,
    COL_STATE,
    COL_RETRY_COUNT,
    COL_MAX_RETRIES,
    COL_REQUEST_ID,
    COL_ELIGIBLE_TS,
    COL_RETRY_AT,
    COL_ERROR_MSG,
    COL_MAX
  };
public:
  ObAllVirtualAiColumnTask() : tasks_(), task_idx_(0), tenant_id_(0),
                               tx_id_(0) {}
  virtual ~ObAllVirtualAiColumnTask() {}
  virtual int inner_open() override;
  virtual int inner_get_next_row(common::ObNewRow *&row) override;
  virtual void reset() override
  {
    tasks_.reuse();
    task_idx_ = 0;
    tenant_id_ = 0;
    tx_id_ = 0;
    common::ObVirtualTableIterator::reset();
  }
private:
  int fill_cell(const share::ai_column::ObAiColumnTaskEntry &entry,
                const int64_t output_idx,
                const uint64_t column_id);
private:
  common::ObArray<share::ai_column::ObAiColumnTaskEntry> tasks_;
  int64_t task_idx_;
  uint64_t tenant_id_;
  uint64_t tx_id_;
};

inline int ObAllVirtualAiColumnTask::inner_open()
{
  int ret = common::OB_SUCCESS;
  if (OB_ISNULL(session_)) {
    ret = common::OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "session is null", K(ret));
  } else {
    // single tenant deployment: the registry rows carry tenant 0, which is
    // treated as a wildcard by the visibility filter
    tenant_id_ = 0;
    tx_id_ = static_cast<uint64_t>(session_->get_tx_id().get_id());
    if (OB_FAIL(share::ai_column::ObAiColumnTaskRegistry::get_visible(tenant_id_, tx_id_,
                                                                     tasks_))) {
      SERVER_LOG(WARN, "failed to load ai column tasks", K(ret), K(tenant_id_), K(tx_id_));
    }
  }
  return ret;
}

inline int ObAllVirtualAiColumnTask::fill_cell(
    const share::ai_column::ObAiColumnTaskEntry &entry,
    const int64_t output_idx,
    const uint64_t column_id)
{
  int ret = common::OB_SUCCESS;
  common::ObObj &cell = cur_row_.cells_[output_idx];
  const uint64_t idx = column_id - common::OB_APP_MIN_COLUMN_ID;
  switch (idx) {
    case COL_TENANT_ID:
      cell.set_int(static_cast<int64_t>(entry.tenant_id_));
      break;
    case COL_TASK_ID:
      cell.set_int(entry.task_id_);
      break;
    case COL_TABLE_ID:
      cell.set_int(static_cast<int64_t>(entry.table_id_));
      break;
    case COL_COLUMN_ID:
      cell.set_int(static_cast<int64_t>(entry.column_id_));
      break;
    case COL_DATABASE_NAME:
      cell.set_varchar(common::ObString(static_cast<int32_t>(strlen(entry.database_name_)),
                                       entry.database_name_));
      break;
    case COL_TABLE_NAME:
      cell.set_varchar(common::ObString(static_cast<int32_t>(strlen(entry.table_name_)),
                                       entry.table_name_));
      break;
    case COL_COLUMN_NAME:
      cell.set_varchar(common::ObString(static_cast<int32_t>(strlen(entry.column_name_)),
                                       entry.column_name_));
      break;
    case COL_ROW_KEY:
    case COL_ROWKEY_PREDICATE:
      cell.set_varchar(common::ObString(static_cast<int32_t>(strlen(entry.rowkey_pred_)),
                                       entry.rowkey_pred_));
      break;
    case COL_STATE:
      cell.set_int(entry.state_);
      break;
    case COL_RETRY_COUNT:
      cell.set_int(entry.retry_count_);
      break;
    case COL_MAX_RETRIES:
      cell.set_int(entry.max_retries_);
      break;
    case COL_REQUEST_ID:
      cell.set_varchar(common::ObString(static_cast<int32_t>(strlen(entry.request_id_)),
                                       entry.request_id_));
      break;
    case COL_ELIGIBLE_TS:
      cell.set_timestamp(entry.eligible_ts_);
      break;
    case COL_RETRY_AT:
      cell.set_timestamp(entry.retry_at_);
      break;
    case COL_ERROR_MSG:
      cell.set_varchar(common::ObString(static_cast<int32_t>(strlen(entry.error_msg_)),
                                       entry.error_msg_));
      break;
    default:
      ret = common::OB_ERR_UNEXPECTED;
      SERVER_LOG(WARN, "unexpected column id", K(ret), K(column_id), K(idx));
      break;
  }
  return ret;
}

inline int ObAllVirtualAiColumnTask::inner_get_next_row(common::ObNewRow *&row)
{
  int ret = common::OB_SUCCESS;
  if (task_idx_ >= tasks_.count()) {
    ret = common::OB_ITER_END;
  } else {
    const int64_t output_count = output_column_ids_.count();
    for (int64_t i = 0; common::OB_SUCCESS == ret && i < output_count; ++i) {
      const uint64_t column_id = output_column_ids_.at(i);
      if (common::OB_SUCCESS != (ret = fill_cell(tasks_.at(task_idx_), i, column_id))) {
        break;
      }
    }
    if (common::OB_SUCCESS == ret) {
      cur_row_.count_ = output_count;
      row = &cur_row_;
      ++task_idx_;
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_VIRTUAL_TABLE_OB_ALL_VIRTUAL_AI_COLUMN_TASK_H_
