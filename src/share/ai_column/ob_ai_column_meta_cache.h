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

#ifndef OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_META_CACHE_H_
#define OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_META_CACHE_H_

#include <pthread.h>
#include <string.h>
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "share/ai_column/ob_ai_column_task_registry.h"

namespace oceanbase
{
namespace share
{
namespace ai_column
{

static const int64_t AI_META_MAX_TABLES = 64;
static const int64_t AI_META_MAX_COLUMNS = 8;
static const int64_t AI_META_MAX_PK = 8;
static const int64_t AI_META_MAX_DEPS = 16;

// One AI column of one table, as needed by the DML side and by the worker.
struct ObAiColumnMeta
{
  uint64_t column_id_;
  int64_t max_retries_;
  bool is_embed_;
  char column_name_[AI_TASK_NAME_LEN];
  // Expression ready to be executed by the worker, e.g.
  //   ai_complete('model', concat('Classify: ', content), '{"system":"..."}')
  char expression_[AI_TASK_EXPR_LEN];
  char instruction_[AI_TASK_SYSTEM_LEN];
  char deps_[AI_TASK_ROWKEY_LEN];
  uint64_t dep_ids_[AI_META_MAX_DEPS];
  int64_t dep_count_;
};

// Everything the DML path needs to know about a table that owns AI columns.
struct ObAiColumnTableMeta
{
  uint64_t table_id_;
  uint64_t database_id_;
  uint64_t display_table_id_;
  uint64_t display_database_id_;
  int64_t column_count_;
  int64_t pk_count_;
  uint64_t pk_ids_[AI_META_MAX_PK];
  char pk_names_[AI_META_MAX_PK][AI_TASK_NAME_LEN];
  char table_name_[AI_TASK_NAME_LEN];
  ObAiColumnMeta columns_[AI_META_MAX_COLUMNS];
};

class ObAiColumnMetaCache
{
public:
  // Insert or replace the meta of one table.
  static int put(const ObAiColumnTableMeta &meta);
  static int get(const uint64_t table_id, ObAiColumnTableMeta &meta, bool &found);
  static int remove(const uint64_t table_id);
  static void clear();
private:
  static ObAiColumnTableMeta *metas()
  {
    static ObAiColumnTableMeta s_metas[AI_META_MAX_TABLES];
    return s_metas;
  }
  static pthread_mutex_t *mutex()
  {
    static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
    return &s_mutex;
  }
};

inline int ObAiColumnMetaCache::put(const ObAiColumnTableMeta &meta)
{
  int ret = OB_SUCCESS;
  pthread_mutex_lock(mutex());
  {
    ObAiColumnTableMeta *arr = metas();
    int64_t slot = -1;
    for (int64_t i = 0; i < AI_META_MAX_TABLES && slot < 0; ++i) {
      if (0 == arr[i].table_id_ || arr[i].table_id_ == meta.table_id_) {
        slot = i;
      }
    }
    if (slot < 0) {
      slot = AI_META_MAX_TABLES - 1;
    }
    arr[slot].table_id_ = 0;
    arr[slot] = meta;
  }
  pthread_mutex_unlock(mutex());
  return ret;
}

inline int ObAiColumnMetaCache::get(const uint64_t table_id, ObAiColumnTableMeta &meta,
                                    bool &found)
{
  int ret = OB_SUCCESS;
  found = false;
  pthread_mutex_lock(mutex());
  {
    ObAiColumnTableMeta *arr = metas();
    for (int64_t i = 0; i < AI_META_MAX_TABLES && !found; ++i) {
      if (0 != arr[i].table_id_ && arr[i].table_id_ == table_id) {
        meta = arr[i];
        found = true;
      }
    }
  }
  pthread_mutex_unlock(mutex());
  return ret;
}

inline int ObAiColumnMetaCache::remove(const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  pthread_mutex_lock(mutex());
  {
    ObAiColumnTableMeta *arr = metas();
    for (int64_t i = 0; i < AI_META_MAX_TABLES; ++i) {
      if (arr[i].table_id_ == table_id) {
        arr[i].table_id_ = 0;
      }
    }
  }
  pthread_mutex_unlock(mutex());
  return ret;
}

inline void ObAiColumnMetaCache::clear()
{
  pthread_mutex_lock(mutex());
  memset(static_cast<void *>(metas()), 0, sizeof(ObAiColumnTableMeta) * AI_META_MAX_TABLES);
  pthread_mutex_unlock(mutex());
}

} // namespace ai_column
} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_META_CACHE_H_
