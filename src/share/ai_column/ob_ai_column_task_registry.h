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

#ifndef OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_TASK_REGISTRY_H_
#define OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_TASK_REGISTRY_H_

#include <pthread.h>
#include <string.h>
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_iarray.h"
#include "lib/atomic/ob_atomic.h"
#include "lib/time/ob_time_utility.h"

namespace oceanbase
{
namespace share
{
namespace ai_column
{

// AI column task rows live in an in-memory registry.  The visible surface is a
// virtual table (oceanbase.__all_ai_column_task) which reports
//   * rows created by the current transaction (so that the writer sees its own
//     uncommitted invalidation, exactly like a real table write would), and
//   * rows whose producing transaction has already been observed as committed
//     (so that a background worker / any other session can see pending work).
// A rolled back transaction simply never becomes committed, therefore its rows
// disappear together with the transaction.
static const int64_t AI_TASK_MAX_ENTRIES = 512;
static const int64_t AI_TASK_NAME_LEN = 128;
static const int64_t AI_TASK_ROWKEY_LEN = 512;
static const int64_t AI_TASK_EXPR_LEN = 4096;
static const int64_t AI_TASK_REQUEST_LEN = 128;
static const int64_t AI_TASK_MSG_LEN = 512;
static const int64_t AI_TASK_SYSTEM_LEN = 256;
// Give up waiting for a row whose AI value never shows up as NULL.  Such a row
// belongs either to a rolled back transaction or to a superseded request.
static const int64_t AI_TASK_MAX_ZERO_HIT = 60;

enum ObAiColumnTaskState
{
  AI_TASK_PENDING = 0,
  AI_TASK_RUNNING = 1,
  AI_TASK_READY = 2,
  AI_TASK_RETRY_WAIT = 3,
  AI_TASK_FAILED = 4,
};

struct ObAiColumnTaskEntry
{
  uint64_t tenant_id_;
  uint64_t tx_id_;
  uint64_t table_id_;
  uint64_t database_id_;
  uint64_t column_id_;
  int64_t task_id_;
  int64_t state_;
  int64_t retry_count_;
  int64_t max_retries_;
  int64_t gmt_create_;
  int64_t eligible_ts_;
  int64_t retry_at_;
  int64_t zero_hit_count_;
  bool committed_;
  bool is_virtual_generated_;
  char database_name_[AI_TASK_NAME_LEN];
  char table_name_[AI_TASK_NAME_LEN];
  char column_name_[AI_TASK_NAME_LEN];
  char rowkey_pred_[AI_TASK_ROWKEY_LEN];
  char request_id_[AI_TASK_REQUEST_LEN];
  char expression_[AI_TASK_EXPR_LEN];
  char system_prompt_[AI_TASK_SYSTEM_LEN];
  char error_msg_[AI_TASK_MSG_LEN];
};

class ObAiColumnTaskRegistry
{
public:
  static ObAiColumnTaskRegistry &get_instance();
  // Register one task row.  A task row is keyed by
  // (table_id, column_id, rowkey_pred); registering a new one replaces the
  // previous row of the same key so that a row never accumulates tasks.
  static int add_task(ObAiColumnTaskEntry &entry);
  static int remove_task(const int64_t task_id);
  static int remove_by_rowkey(const uint64_t table_id, const char *rowkey_pred);
  static int remove_by_table(const uint64_t table_id);
  static int claim_runnable(const int64_t now, common::ObIArray<ObAiColumnTaskEntry> &out,
                            const int64_t limit);
  static int mark_committed(const int64_t task_id);
  static int mark_zero_hit(const int64_t task_id, const int64_t now);
  static int mark_error(const int64_t task_id, const int64_t now, const char *error_msg);
  static int mark_done(const int64_t task_id);
  // Rows visible to a session: its own uncommitted rows plus committed rows.
  static int get_visible(const uint64_t tenant_id, const uint64_t tx_id,
                         common::ObIArray<ObAiColumnTaskEntry> &out);
  static int count_for_column(const uint64_t table_id, const uint64_t column_id,
                              int64_t &count);
  static int count_for_table(const uint64_t table_id, int64_t &count);
  static int has_open_task(const uint64_t table_id, const uint64_t column_id, bool &has);
  static int get_task(const int64_t task_id, ObAiColumnTaskEntry &entry, bool &found);
  // Number of rows of (table, column) whose AI value is still missing.
  static int get_pending_count(const uint64_t table_id, const uint64_t column_id,
                               int64_t &pending);
  static int64_t get_count();

private:
  static ObAiColumnTaskEntry *entries()
  {
    // POD array: constant initialised, no guard variable and no dynamic ctor.
    static ObAiColumnTaskEntry s_entries[AI_TASK_MAX_ENTRIES];
    return s_entries;
  }
  static pthread_mutex_t *mutex()
  {
    static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
    return &s_mutex;
  }
  static int64_t *id_seq()
  {
    static int64_t s_seq = 0;
    return &s_seq;
  }
  static int64_t *count()
  {
    static int64_t s_count = 0;
    return &s_count;
  }

  // Column level counter of rows whose AI value is still missing.  It is
  // incremented when a task row is created and decremented when the row has
  // been computed, so it also covers rows whose task was dropped after a
  // terminal failure (their AI value stays NULL, hence the column stays
  // incomplete).
  struct ColumnPending
  {
    uint64_t table_id_;
    uint64_t column_id_;
    int64_t pending_;
  };
  static const int64_t MAX_PENDING_SLOTS = 64;
  static ColumnPending *pending_counter()
  {
    static ColumnPending s_pending[MAX_PENDING_SLOTS];
    return s_pending;
  }
  static void bump_pending(const uint64_t table_id, const uint64_t column_id,
                           const int64_t delta)
  {
    ColumnPending *slots = pending_counter();
    int64_t free_slot = -1;
    for (int64_t i = 0; i < MAX_PENDING_SLOTS; ++i) {
      if (slots[i].table_id_ == table_id && slots[i].column_id_ == column_id) {
        slots[i].pending_ += delta;
        if (slots[i].pending_ < 0) {
          slots[i].pending_ = 0;
        }
        return;
      } else if (0 == slots[i].table_id_ && free_slot < 0) {
        free_slot = i;
      }
    }
    if (free_slot >= 0) {
      slots[free_slot].table_id_ = table_id;
      slots[free_slot].column_id_ = column_id;
      slots[free_slot].pending_ = (delta > 0) ? delta : 0;
    }
  }
  // caller must hold the lock
  static int64_t find_index(const int64_t task_id)
  {
    int64_t idx = -1;
    ObAiColumnTaskEntry *arr = entries();
    for (int64_t i = 0; i < AI_TASK_MAX_ENTRIES && idx < 0; ++i) {
      if (0 != arr[i].task_id_ && arr[i].task_id_ == task_id) {
        idx = i;
      }
    }
    return idx;
  }
  static bool same_rowkey(const ObAiColumnTaskEntry &a, const char *rowkey_pred)
  {
    return 0 == strncmp(a.rowkey_pred_, rowkey_pred, AI_TASK_ROWKEY_LEN - 1);
  }
  static void clear_entry(ObAiColumnTaskEntry &entry)
  {
    memset(static_cast<void *>(&entry), 0, sizeof(entry));
  }
  static int alloc_slot(int64_t &slot)
  {
    int ret = common::OB_SUCCESS;
    ObAiColumnTaskEntry *arr = entries();
    slot = -1;
    for (int64_t i = 0; i < AI_TASK_MAX_ENTRIES && slot < 0; ++i) {
      if (0 == arr[i].task_id_) {
        slot = i;
      }
    }
    if (slot < 0) {
      // Evict the oldest entry so that the registry can never grow unbounded.
      int64_t oldest = 0;
      for (int64_t i = 1; i < AI_TASK_MAX_ENTRIES; ++i) {
        if (arr[i].task_id_ < arr[oldest].task_id_) {
          oldest = i;
        }
      }
      slot = oldest;
      clear_entry(arr[slot]);
    }
    return ret;
  }
  static void copy_str(char *dst, const int64_t dst_len, const common::ObString &src)
  {
    int64_t len = src.length();
    if (len > dst_len - 1) {
      len = dst_len - 1;
    }
    if (len > 0) {
      memcpy(dst, src.ptr(), len);
    }
    dst[len] = '\0';
  }
  static void copy_cstr(char *dst, const int64_t dst_len, const char *src)
  {
    if (NULL == src) {
      dst[0] = '\0';
    } else {
      int64_t len = static_cast<int64_t>(strlen(src));
      if (len > dst_len - 1) {
        len = dst_len - 1;
      }
      memcpy(dst, src, len);
      dst[len] = '\0';
    }
  }
  static void lock()
  {
    pthread_mutex_lock(mutex());
  }
  static void unlock()
  {
    pthread_mutex_unlock(mutex());
  }
};

inline ObAiColumnTaskRegistry &ObAiColumnTaskRegistry::get_instance()
{
  static ObAiColumnTaskRegistry s_instance;
  return s_instance;
}

inline int64_t ObAiColumnTaskRegistry::get_count()
{
  lock();
  const int64_t cnt = *count();
  unlock();
  return cnt;
}

inline int ObAiColumnTaskRegistry::add_task(ObAiColumnTaskEntry &entry)
{
  int ret = common::OB_SUCCESS;
  lock();
  {
    ObAiColumnTaskEntry *arr = entries();
    int64_t replaced_cnt = 0;
    // replace the previous row of the same (table, column, rowkey)
    for (int64_t i = 0; i < AI_TASK_MAX_ENTRIES; ++i) {
      if (0 != arr[i].task_id_
          && arr[i].table_id_ == entry.table_id_
          && arr[i].column_id_ == entry.column_id_
          && same_rowkey(arr[i], entry.rowkey_pred_)) {
        clear_entry(arr[i]);
        ++replaced_cnt;
        if (*count() > 0) {
          --(*count());
        }
      }
    }
    int64_t slot = -1;
    if (common::OB_SUCCESS == ret) {
      ret = alloc_slot(slot);
    }
    const bool is_new_row = (0 == replaced_cnt);
    if (common::OB_SUCCESS == ret) {
      const int64_t now = common::ObTimeUtility::current_time();
      entry.task_id_ = ATOMIC_AAF(id_seq(), 1);
      entry.gmt_create_ = now;
      if (0 == entry.eligible_ts_) {
        entry.eligible_ts_ = now;
      }
      entry.state_ = AI_TASK_PENDING;
      entry.retry_count_ = 0;
      entry.zero_hit_count_ = 0;
      entry.committed_ = false;
      entry.retry_at_ = 0;
      entry.error_msg_[0] = '\0';
      arr[slot] = entry;
      ++(*count());
      if (0 == replaced_cnt) {
        bump_pending(entry.table_id_, entry.column_id_, 1);
      }
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::remove_task(const int64_t task_id)
{
  int ret = common::OB_SUCCESS;
  lock();
  {
    const int64_t idx = find_index(task_id);
    if (idx >= 0) {
      clear_entry(entries()[idx]);
      if (*count() > 0) {
        --(*count());
      }
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::remove_by_rowkey(const uint64_t table_id,
                                                    const char *rowkey_pred)
{
  int ret = common::OB_SUCCESS;
  if (NULL == rowkey_pred) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    lock();
    {
      ObAiColumnTaskEntry *arr = entries();
      for (int64_t i = 0; i < AI_TASK_MAX_ENTRIES; ++i) {
        if (0 != arr[i].task_id_ && arr[i].table_id_ == table_id
            && same_rowkey(arr[i], rowkey_pred)) {
          bump_pending(arr[i].table_id_, arr[i].column_id_, -1);
          clear_entry(arr[i]);
          if (*count() > 0) {
            --(*count());
          }
        }
      }
    }
    unlock();
  }
  return ret;
}

inline int ObAiColumnTaskRegistry::remove_by_table(const uint64_t table_id)
{
  int ret = common::OB_SUCCESS;
  lock();
  {
    ObAiColumnTaskEntry *arr = entries();
    for (int64_t i = 0; i < AI_TASK_MAX_ENTRIES; ++i) {
      if (0 != arr[i].task_id_ && arr[i].table_id_ == table_id) {
        clear_entry(arr[i]);
        if (*count() > 0) {
          --(*count());
        }
      }
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::claim_runnable(const int64_t now,
                                                  common::ObIArray<ObAiColumnTaskEntry> &out,
                                                  const int64_t limit)
{
  int ret = common::OB_SUCCESS;
  lock();
  {
    ObAiColumnTaskEntry *arr = entries();
    int64_t claimed = 0;
    for (int64_t i = 0; i < AI_TASK_MAX_ENTRIES && claimed < limit; ++i) {
      ObAiColumnTaskEntry &e = arr[i];
      if (0 == e.task_id_) {
        continue;
      }
      const bool runnable = (AI_TASK_PENDING == e.state_)
          || (AI_TASK_RETRY_WAIT == e.state_ && e.retry_at_ <= now);
      if (runnable) {
        e.state_ = AI_TASK_RUNNING;
        if (common::OB_SUCCESS != (ret = out.push_back(e))) {
          break;
        } else {
          ++claimed;
        }
      }
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::mark_committed(const int64_t task_id)
{
  int ret = common::OB_SUCCESS;
  lock();
  {
    const int64_t idx = find_index(task_id);
    if (idx >= 0) {
      entries()[idx].committed_ = true;
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::mark_zero_hit(const int64_t task_id, const int64_t now)
{
  int ret = common::OB_SUCCESS;
  UNUSED(now);
  lock();
  {
    const int64_t idx = find_index(task_id);
    if (idx >= 0) {
      ObAiColumnTaskEntry &e = entries()[idx];
      ++e.zero_hit_count_;
      e.state_ = AI_TASK_PENDING;
      if (e.zero_hit_count_ >= AI_TASK_MAX_ZERO_HIT) {
        clear_entry(e);
        if (*count() > 0) {
          --(*count());
        }
      }
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::mark_error(const int64_t task_id, const int64_t now,
                                              const char *error_msg)
{
  int ret = common::OB_SUCCESS;
  lock();
  {
    const int64_t idx = find_index(task_id);
    if (idx >= 0) {
      ObAiColumnTaskEntry &e = entries()[idx];
      ++e.retry_count_;
      e.committed_ = true;
      copy_cstr(e.error_msg_, AI_TASK_MSG_LEN, error_msg);
      if (e.retry_count_ > e.max_retries_) {
        clear_entry(e);
        if (*count() > 0) {
          --(*count());
        }
      } else {
        e.state_ = AI_TASK_RETRY_WAIT;
        e.retry_at_ = now + 200 * 1000L;
      }
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::mark_done(const int64_t task_id)
{
  int ret = common::OB_SUCCESS;
  lock();
  {
    const int64_t idx = find_index(task_id);
    if (idx >= 0) {
      bump_pending(entries()[idx].table_id_, entries()[idx].column_id_, -1);
      clear_entry(entries()[idx]);
      if (*count() > 0) {
        --(*count());
      }
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::get_visible(const uint64_t tenant_id, const uint64_t tx_id,
                                               common::ObIArray<ObAiColumnTaskEntry> &out)
{
  int ret = common::OB_SUCCESS;
  lock();
  {
    ObAiColumnTaskEntry *arr = entries();
    for (int64_t i = 0; i < AI_TASK_MAX_ENTRIES; ++i) {
      const ObAiColumnTaskEntry &e = arr[i];
      if (0 == e.task_id_ || (0 != e.tenant_id_ && e.tenant_id_ != tenant_id)) {
        continue;
      }
      const bool mine = (0 != tx_id && e.tx_id_ == tx_id);
      if (mine || e.committed_) {
        if (common::OB_SUCCESS != (ret = out.push_back(e))) {
          break;
        }
      }
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::count_for_column(const uint64_t table_id,
                                                    const uint64_t column_id,
                                                    int64_t &count_out)
{
  int ret = common::OB_SUCCESS;
  count_out = 0;
  lock();
  {
    ObAiColumnTaskEntry *arr = entries();
    for (int64_t i = 0; i < AI_TASK_MAX_ENTRIES; ++i) {
      if (0 != arr[i].task_id_ && arr[i].table_id_ == table_id
          && (0 == column_id || arr[i].column_id_ == column_id)) {
        ++count_out;
      }
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::count_for_table(const uint64_t table_id, int64_t &count_out)
{
  return count_for_column(table_id, 0, count_out);
}

inline int ObAiColumnTaskRegistry::has_open_task(const uint64_t table_id,
                                                 const uint64_t column_id,
                                                 bool &has)
{
  int64_t cnt = 0;
  int ret = count_for_column(table_id, column_id, cnt);
  has = (cnt > 0);
  return ret;
}

inline int ObAiColumnTaskRegistry::get_task(const int64_t task_id, ObAiColumnTaskEntry &entry,
                                            bool &found)
{
  int ret = common::OB_SUCCESS;
  found = false;
  lock();
  {
    const int64_t idx = find_index(task_id);
    if (idx >= 0) {
      entry = entries()[idx];
      found = true;
    }
  }
  unlock();
  return ret;
}

inline int ObAiColumnTaskRegistry::get_pending_count(const uint64_t table_id,
                                                     const uint64_t column_id,
                                                     int64_t &pending)
{
  int ret = common::OB_SUCCESS;
  pending = 0;
  ColumnPending *slots = pending_counter();
  for (int64_t i = 0; i < MAX_PENDING_SLOTS; ++i) {
    if (slots[i].table_id_ == table_id && slots[i].column_id_ == column_id) {
      pending = slots[i].pending_;
    }
  }
  return ret;
}

} // namespace ai_column
} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_TASK_REGISTRY_H_
