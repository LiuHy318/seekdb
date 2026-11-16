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

#ifndef OCEANBASE_SQL_DAS_OB_DAS_AI_COLUMN_OBSERVER_H_
#define OCEANBASE_SQL_DAS_OB_DAS_AI_COLUMN_OBSERVER_H_

#include "lib/ob_define.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_array.h"
#include "lib/container/ob_se_array.h"
#include "lib/atomic/ob_atomic.h"
#include "common/object/ob_obj_type.h"
#include "storage/blocksstable/ob_datum_row.h"
#include "share/ai_column/ob_ai_column_task_registry.h"
#include "share/ai_column/ob_ai_column_meta_cache.h"
#include "data_plane/transaction/ob_tx_desc_access.h"
#include "sql/das/ob_das_dml_ctx_define.h"

namespace oceanbase
{
namespace sql
{

enum ObAiColumnDmlMode
{
  AI_COLUMN_DML_INSERT = 0,
  AI_COLUMN_DML_UPDATE = 1,
  AI_COLUMN_DML_DELETE = 2,
};

struct ObAiColumnOldRow
{
  common::ObString rowkey_pred_;
  uint64_t non_null_mask_;
};

// Builds and maintains AI column task rows out of the rows a DAS dml operator is
// about to write.  The observer never fails the dml statement: any bookkeeping
// problem is logged and ignored.
class ObAiColumnRowObserver
{
public:
  ObAiColumnRowObserver()
    : inited_(false), mode_(AI_COLUMN_DML_INSERT), ctdef_(nullptr), tx_id_(0),
      has_meta_(false), single_row_mode_(false), force_invalidate_(false),
      new_row_idx_(0), request_seq_(0),
      arena_("AiColumnObs"),
      old_rows_()
  {}
  ~ObAiColumnRowObserver() {}
  int init(const ObDASDMLBaseCtDef *ctdef,
           const transaction::ObTxDesc *tx_desc,
           const ObAiColumnDmlMode mode);
  int init(const ObDASDMLBaseCtDef *ctdef,
           const uint64_t tx_id,
           const ObAiColumnDmlMode mode);
  bool need_observe() const { return has_meta_; }
  void set_single_row_mode(const bool v) { single_row_mode_ = v; }
  void set_force_invalidate(const bool v) { force_invalidate_ = v; }
  const share::ai_column::ObAiColumnTableMeta &get_meta() const { return meta_; }
  int observe(const common::ObDatum *datums, const int64_t count, const bool is_old_row);
  int observe(blocksstable::ObDatumRow &row, const bool is_old_row)
  {
    return observe(row.storage_datums_, row.count_, is_old_row);
  }
private:
  int find_column_idx(const uint64_t column_id, int64_t &idx) const;
  bool column_updated(const uint64_t column_id) const;
  bool need_invalidate(const share::ai_column::ObAiColumnMeta &col) const;
  void invalidate_ai_columns(const common::ObDatum *datums, const int64_t count);
  int build_rowkey_predicate(const common::ObDatum *datums, const int64_t count,
                             common::ObString &predicate);
  int observe_insert_or_update(const common::ObDatum *datums, const int64_t count,
                               const ObAiColumnOldRow *old_row);
  int observe_delete(const common::ObDatum *datums, const int64_t count);
  int format_datum(const ObObjMeta &meta, const ObDatum &datum,
                   common::ObSqlString &out) const;
  void next_request_id(char *buf, const int64_t buf_len);
private:
  bool inited_;
  ObAiColumnDmlMode mode_;
  const ObDASDMLBaseCtDef *ctdef_;
  uint64_t tx_id_;
  bool has_meta_;
  bool single_row_mode_;
  bool force_invalidate_;
  share::ai_column::ObAiColumnTableMeta meta_;
  int64_t new_row_idx_;
  int64_t request_seq_;
  common::ObArenaAllocator arena_;
  common::ObArray<ObAiColumnOldRow> old_rows_;
};

inline int ObAiColumnRowObserver::init(const ObDASDMLBaseCtDef *ctdef,
                                       const transaction::ObTxDesc *tx_desc,
                                       const ObAiColumnDmlMode mode)
{
  int ret = common::OB_SUCCESS;
  if (OB_ISNULL(ctdef) || OB_ISNULL(tx_desc)) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    ctdef_ = ctdef;
    mode_ = mode;
    tx_id_ = static_cast<uint64_t>(data_plane::tx_desc_id(tx_desc).get_id());
    bool found = false;
    const uint64_t real_table_id = ctdef->table_param_.get_data_table().get_table_id();
    if (OB_FAIL(share::ai_column::ObAiColumnMetaCache::get(real_table_id, meta_, found))) {
      LOG_WARN("failed to load ai column meta", K(ret), "table_id", ctdef->table_id_);
    } else {
      has_meta_ = found && meta_.column_count_ > 0;
    }
    inited_ = true;
  }
  return ret;
}

inline int ObAiColumnRowObserver::find_column_idx(const uint64_t column_id, int64_t &idx) const
{
  int ret = common::OB_SUCCESS;
  idx = -1;
  if (OB_ISNULL(ctdef_)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; i < ctdef_->column_ids_.count() && idx < 0; ++i) {
      if (ctdef_->column_ids_.at(i) == column_id) {
        idx = i;
      }
    }
  }
  return ret;
}

inline int ObAiColumnRowObserver::init(const ObDASDMLBaseCtDef *ctdef,
                                       const uint64_t tx_id,
                                       const ObAiColumnDmlMode mode)
{
  int ret = common::OB_SUCCESS;
  if (OB_ISNULL(ctdef)) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    ctdef_ = ctdef;
    mode_ = mode;
    tx_id_ = tx_id;
    bool found = false;
    const uint64_t real_table_id = ctdef->table_param_.get_data_table().get_table_id();
    if (OB_FAIL(share::ai_column::ObAiColumnMetaCache::get(real_table_id, meta_, found))) {
    } else {
      has_meta_ = found && meta_.column_count_ > 0;
    }
    inited_ = true;
  }
  return ret;
}

inline bool ObAiColumnRowObserver::column_updated(const uint64_t column_id) const
{
  bool updated = false;
  if (AI_COLUMN_DML_UPDATE == mode_ && OB_NOT_NULL(ctdef_)) {
    const ObDASUpdCtDef *upd_ctdef = static_cast<const ObDASUpdCtDef *>(ctdef_);
    for (int64_t i = 0; i < upd_ctdef->updated_column_ids_.count() && !updated; ++i) {
      if (upd_ctdef->updated_column_ids_.at(i) == column_id) {
        updated = true;
      }
    }
  }
  return updated;
}

// A row must be invalidated when one of the AI column dependencies or one of the
// primary key columns is part of the update statement.
inline bool ObAiColumnRowObserver::need_invalidate(
    const share::ai_column::ObAiColumnMeta &col) const
{
  bool invalidate = false;
  for (int64_t d = 0; !invalidate && d < col.dep_count_; ++d) {
    invalidate = column_updated(col.dep_ids_[d]);
  }
  for (int64_t p = 0; !invalidate && p < meta_.pk_count_; ++p) {
    invalidate = column_updated(meta_.pk_ids_[p]);
  }
  return invalidate;
}

inline void ObAiColumnRowObserver::invalidate_ai_columns(const common::ObDatum *datums,
                                                         const int64_t count)
{
  for (int64_t i = 0; i < meta_.column_count_; ++i) {
    const share::ai_column::ObAiColumnMeta &col = meta_.columns_[i];
    if (!force_invalidate_ && !need_invalidate(col)) {
      continue;
    }
    int64_t idx = -1;
    if (OB_SUCCESS == find_column_idx(col.column_id_, idx)
        && idx >= 0 && idx < count) {
      const_cast<common::ObDatum *>(datums)[idx].set_null();
    }
  }
}

inline int ObAiColumnRowObserver::format_datum(const ObObjMeta &meta, const ObDatum &datum,
                                               common::ObSqlString &out) const
{
  int ret = common::OB_SUCCESS;
  if (datum.is_null()) {
    ret = out.append("null");
  } else {
    const ObObjType type = meta.get_type();
    if (ob_is_integer_type(type) || ob_is_bit_tc(type)) {
      ret = out.append_fmt("%ld", datum.get_int());
    } else if (ob_is_unsigned_type(type)) {
      ret = out.append_fmt("%lu", datum.get_uint64());
    } else if (ob_is_string_tc(type) || ob_is_text_tc(type) || ob_is_json(type)) {
      const common::ObString str = datum.get_string();
      if (OB_FAIL(out.append("'"))) {
      } else {
        for (int64_t i = 0; common::OB_SUCCESS == ret && i < str.length(); ++i) {
          const char c = str.ptr()[i];
          if ('\'' == c) {
            ret = out.append("''");
          } else if ('\\' == c) {
            ret = out.append("\\\\");
          } else {
            ret = out.append(&c, 1);
          }
        }
        if (common::OB_SUCCESS == ret) {
          ret = out.append("'");
        }
      }
    } else if (ob_is_temporal_type(type)) {
      ret = out.append_fmt("%ld", datum.get_timestamp());
    } else {
      ret = out.append_fmt("%ld", datum.get_int());
    }
  }
  return ret;
}

inline int ObAiColumnRowObserver::build_rowkey_predicate(const common::ObDatum *datums,
                                                        const int64_t count,
                                                        common::ObString &predicate)
{
  int ret = common::OB_SUCCESS;
  common::ObSqlString sql;
  predicate.reset();
  if (0 == meta_.pk_count_) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (count < ctdef_->column_ids_.count()) {
    // the row does not carry the full write layout (e.g. projected old rows)
    ret = common::OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; common::OB_SUCCESS == ret && i < meta_.pk_count_; ++i) {
      int64_t idx = -1;
      if (OB_FAIL(find_column_idx(meta_.pk_ids_[i], idx))) {
      } else if (idx < 0) {
        ret = common::OB_ERR_UNEXPECTED;
      } else {
        const ObObjMeta meta = (idx < ctdef_->column_types_.count())
            ? ctdef_->column_types_.at(idx) : ObObjMeta();
        if (OB_FAIL(sql.append_fmt("%s`%s` = ", (0 == i) ? "" : " and ",
                                   meta_.pk_names_[i]))) {
        } else if (OB_FAIL(format_datum(meta, datums[idx], sql))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      char *buf = static_cast<char *>(arena_.alloc(sql.length() + 1));
      if (OB_ISNULL(buf)) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, sql.ptr(), sql.length());
        buf[sql.length()] = '\0';
        predicate.assign_ptr(buf, static_cast<int32_t>(sql.length()));
      }
    }
  }
  return ret;
}

inline void ObAiColumnRowObserver::next_request_id(char *buf, const int64_t buf_len)
{
  const int64_t seq = ATOMIC_AAF(&request_seq_, 1);
  snprintf(buf, buf_len, "%lu_%ld", tx_id_, seq);
}

inline int ObAiColumnRowObserver::observe_insert_or_update(const common::ObDatum *datums,
                                                           const int64_t count,
                                                           const ObAiColumnOldRow *old_row)
{
  int ret = common::OB_SUCCESS;
  common::ObString predicate;
  char request_id[share::ai_column::AI_TASK_REQUEST_LEN];
  request_id[0] = '\0';
  if (AI_COLUMN_DML_UPDATE == mode_ || force_invalidate_) {
    // the invalidation is written by the very same update statement
    invalidate_ai_columns(datums, count);
  }
  if (OB_FAIL(build_rowkey_predicate(datums, count, predicate))) {
    LOG_DEBUG("skip ai column task, rowkey not available", K(ret),
              "table_id", meta_.table_id_);
    ret = common::OB_SUCCESS;
  } else {
    next_request_id(request_id, sizeof(request_id));
    for (int64_t i = 0; common::OB_SUCCESS == ret && i < meta_.column_count_; ++i) {
      const share::ai_column::ObAiColumnMeta &col = meta_.columns_[i];
      int64_t idx = -1;
      if (OB_FAIL(find_column_idx(col.column_id_, idx))) {
      } else if (idx < 0 || idx >= count) {
        continue;
      } else if (!datums[idx].is_null()) {
        // the AI value is provided by the statement itself: the row is complete
        share::ai_column::ObAiColumnTaskRegistry::mark_present(
            meta_.display_table_id_, col.column_id_, predicate.ptr());
        continue;
      } else {
        bool need_task = false;
        if (AI_COLUMN_DML_INSERT == mode_) {
          need_task = true;
        } else if (need_invalidate(col)) {
          need_task = true;
        } else {
          const bool old_was_null = (NULL != old_row)
              && (0 == (old_row->non_null_mask_ & (1UL << i)));
          if (NULL == old_row || !old_was_null) {
            // explicit retry: the value went from non null to null
            need_task = true;
          }
        }
        if (need_task) {
          share::ai_column::ObAiColumnTaskEntry entry;
          MEMSET(static_cast<void *>(&entry), 0, sizeof(entry));
          entry.tenant_id_ = 0;
          entry.tx_id_ = tx_id_;
          entry.table_id_ = meta_.display_table_id_;
          entry.database_id_ = meta_.display_database_id_;
          entry.column_id_ = col.column_id_;
          entry.max_retries_ = col.max_retries_;
          strncpy(entry.column_name_, col.column_name_,
                  share::ai_column::AI_TASK_NAME_LEN - 1);
          strncpy(entry.table_name_, meta_.table_name_,
                  share::ai_column::AI_TASK_NAME_LEN - 1);
          MEMCPY(entry.rowkey_pred_, predicate.ptr(), predicate.length());
          MEMCPY(entry.request_id_, request_id, strlen(request_id));
          strncpy(entry.expression_, col.expression_,
                  share::ai_column::AI_TASK_EXPR_LEN - 1);
          strncpy(entry.system_prompt_, col.instruction_,
                  share::ai_column::AI_TASK_SYSTEM_LEN - 1);
          if (OB_FAIL(share::ai_column::ObAiColumnTaskRegistry::add_task(entry))) {
            LOG_WARN("failed to add ai column task", K(ret), "table_id", meta_.table_id_);
          }
        }
      }
    }
    if (common::OB_SUCCESS == ret && AI_COLUMN_DML_UPDATE == mode_) {
      bool pk_updated = false;
      for (int64_t p = 0; !pk_updated && p < meta_.pk_count_; ++p) {
        pk_updated = column_updated(meta_.pk_ids_[p]);
      }
      if (pk_updated && NULL != old_row && !old_row->rowkey_pred_.empty()) {
        char old_pred[share::ai_column::AI_TASK_ROWKEY_LEN];
        MEMSET(old_pred, 0, sizeof(old_pred));
        const int64_t len = MIN(old_row->rowkey_pred_.length(),
                                static_cast<int64_t>(sizeof(old_pred) - 1));
        MEMCPY(old_pred, old_row->rowkey_pred_.ptr(), len);
        share::ai_column::ObAiColumnTaskRegistry::remove_by_rowkey(meta_.display_table_id_,
                                                                  old_pred);
      }
    }
  }
  return ret;
}

inline int ObAiColumnRowObserver::observe_delete(const common::ObDatum *datums,
                                                 const int64_t count)
{
  int ret = common::OB_SUCCESS;
  common::ObString predicate;
  if (OB_FAIL(build_rowkey_predicate(datums, count, predicate))) {
    ret = common::OB_SUCCESS;
  } else {
    char pred[share::ai_column::AI_TASK_ROWKEY_LEN];
    MEMSET(pred, 0, sizeof(pred));
    const int64_t len = MIN(predicate.length(), static_cast<int64_t>(sizeof(pred) - 1));
    MEMCPY(pred, predicate.ptr(), len);
    share::ai_column::ObAiColumnTaskRegistry::remove_by_rowkey(meta_.display_table_id_, pred);
  }
  return ret;
}

inline int ObAiColumnRowObserver::observe(const common::ObDatum *datums,
                                          const int64_t count,
                                          const bool is_old_row)
{
  int ret = common::OB_SUCCESS;
  if (!has_meta_) {
    // nothing to observe
  } else if (AI_COLUMN_DML_DELETE == mode_) {
    if (OB_FAIL(observe_delete(datums, count))) {
      LOG_DEBUG("failed to observe delete for ai column", K(ret));
      ret = common::OB_SUCCESS;
    }
  } else if (AI_COLUMN_DML_UPDATE == mode_) {
    if (is_old_row && !single_row_mode_) {
      ObAiColumnOldRow old_row;
      old_row.non_null_mask_ = 0;
      common::ObString predicate;
      if (OB_SUCC(build_rowkey_predicate(datums, count, predicate))) {
        old_row.rowkey_pred_ = predicate;
        for (int64_t i = 0; i < meta_.column_count_; ++i) {
          int64_t idx = -1;
          if (OB_FAIL(find_column_idx(meta_.columns_[i].column_id_, idx))) {
          } else if (idx >= 0 && idx < count && !datums[idx].is_null()) {
            old_row.non_null_mask_ |= (1UL << i);
          }
        }
        if (OB_FAIL(old_rows_.push_back(old_row))) {
          LOG_WARN("failed to buffer old row", K(ret));
        }
      }
      ret = common::OB_SUCCESS;
    } else {
      const ObAiColumnOldRow *old_row = (new_row_idx_ < old_rows_.count())
          ? &old_rows_.at(new_row_idx_) : NULL;
      ++new_row_idx_;
      if (OB_FAIL(observe_insert_or_update(datums, count, old_row))) {
        LOG_WARN("failed to observe update for ai column", K(ret));
        ret = common::OB_SUCCESS;
      }
    }
  } else {
    if (OB_FAIL(observe_insert_or_update(datums, count, NULL))) {
      LOG_WARN("failed to observe insert for ai column", K(ret));
      ret = common::OB_SUCCESS;
    }
  }
  return ret;
}

inline bool is_ai_column_table(const uint64_t table_id)
{
  share::ai_column::ObAiColumnTableMeta meta;
  bool found = false;
  share::ai_column::ObAiColumnMetaCache::get(table_id, meta, found);
  return found && meta.column_count_ > 0;
}

// Observe the rows of an insert/delete dml by replaying the write buffer.
// The iterator is rewound again afterwards so the real write starts from the
// first row.
inline void observe_ai_column_rows(const ObDASDMLBaseCtDef &ctdef,
                                   transaction::ObTxDesc *tx_desc,
                                   ObDASDMLIterator &iter,
                                   const ObAiColumnDmlMode mode)
{
  ObAiColumnRowObserver observer;
  int ret = observer.init(&ctdef, tx_desc, mode);
  if (common::OB_SUCCESS != ret || !observer.need_observe()) {
    return;
  }
  blocksstable::ObDatumRow *row = nullptr;
  ret = iter.rewind(&ctdef, nullptr);
  while (common::OB_SUCCESS == ret && common::OB_SUCCESS == (ret = iter.get_next_row(row))) {
    if (OB_NOT_NULL(row)) {
      observer.observe(*row, false);
    }
  }
  iter.rewind(&ctdef, nullptr);
}

// Wrapper used by the update path: it forwards to the DAS update iterator,
// turns every invalidated AI value into NULL inside the row that is about to be
// written (so that the writer sees the invalidation transactionally) and feeds
// the row to the task observer.
class ObAiColumnUpdateRowWrapper : public blocksstable::ObDatumRowIterator
{
public:
  ObAiColumnUpdateRowWrapper()
    : inner_(nullptr), observer_(nullptr), is_old_phase_(true)
  {}
  virtual ~ObAiColumnUpdateRowWrapper() {}
  int init(blocksstable::ObDatumRowIterator *inner, ObAiColumnRowObserver *observer)
  {
    int ret = common::OB_SUCCESS;
    if (OB_ISNULL(inner) || OB_ISNULL(observer)) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      inner_ = inner;
      observer_ = observer;
      is_old_phase_ = true;
    }
    return ret;
  }
  virtual int get_next_row(blocksstable::ObDatumRow *&row) override
  {
    int ret = inner_->get_next_row(row);
    if (common::OB_SUCCESS == ret && OB_NOT_NULL(row) && OB_NOT_NULL(observer_)) {
      const bool is_old = is_old_phase_;
      is_old_phase_ = !is_old_phase_;
      observer_->observe(*row, is_old);
    }
    return ret;
  }
  virtual int get_next_rows(blocksstable::ObDatumRow *&rows, int64_t &row_count) override
  {
    int ret = inner_->get_next_rows(rows, row_count);
    if (common::OB_SUCCESS == ret && row_count > 0 && OB_NOT_NULL(observer_)) {
      const bool is_old = is_old_phase_;
      is_old_phase_ = !is_old_phase_;
      for (int64_t i = 0; i < row_count; ++i) {
        observer_->observe(rows[i], is_old);
      }
    }
    return ret;
  }
  virtual void reset() override
  {
    if (OB_NOT_NULL(inner_)) {
      inner_->reset();
    }
  }
private:
  blocksstable::ObDatumRowIterator *inner_;
  ObAiColumnRowObserver *observer_;
  bool is_old_phase_;
};

// Prepared state of one update statement: the write layout plus the wrapper.
class ObAiColumnUpdateContext
{
public:
  ObAiColumnUpdateContext(const ObDASUpdCtDef &ctdef, transaction::ObTxDesc *tx_desc,
                          blocksstable::ObDatumRowIterator &inner)
    : ctdef_(ctdef), tx_desc_(tx_desc), inner_(inner), observer_(), wrapper_(),
      updated_column_ids_(), iterator_(&inner)
  {}
  int init();
  blocksstable::ObDatumRowIterator *get_iterator() { return iterator_; }
  const common::ObIArray<uint64_t> &get_updated_column_ids() const
  {
    return updated_column_ids_;
  }
private:
  const ObDASUpdCtDef &ctdef_;
  transaction::ObTxDesc *tx_desc_;
  blocksstable::ObDatumRowIterator &inner_;
  ObAiColumnRowObserver observer_;
  ObAiColumnUpdateRowWrapper wrapper_;
  common::ObSEArray<uint64_t, 16> updated_column_ids_;
  blocksstable::ObDatumRowIterator *iterator_;
};

inline int ObAiColumnUpdateContext::init()
{
  int ret = common::OB_SUCCESS;
  if (OB_FAIL(updated_column_ids_.assign(ctdef_.updated_column_ids_))) {
  } else if (!is_ai_column_table(
                 ctdef_.table_param_.get_data_table().get_table_id())) {
  } else if (OB_FAIL(observer_.init(&ctdef_, tx_desc_, AI_COLUMN_DML_UPDATE))) {
  } else if (!observer_.need_observe()) {
  } else {
    const share::ai_column::ObAiColumnTableMeta &meta = observer_.get_meta();
    for (int64_t i = 0; OB_SUCC(ret) && i < meta.column_count_; ++i) {
      const uint64_t column_id = meta.columns_[i].column_id_;
      bool exists = false;
      for (int64_t j = 0; !exists && j < updated_column_ids_.count(); ++j) {
        exists = (updated_column_ids_.at(j) == column_id);
      }
      if (!exists) {
        ret = updated_column_ids_.push_back(column_id);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(wrapper_.init(&inner_, &observer_))) {
    } else {
      iterator_ = &wrapper_;
    }
  }
  return ret;
}

// Entry point used by the dml service: one written row, already serialised into
// the DAS write buffer.
inline void observe_ai_column_datums(const ObDASDMLBaseCtDef &ctdef,
                                     const uint64_t tx_id,
                                     const ObAiColumnDmlMode mode,
                                     common::ObDatum *datums,
                                     const int64_t count)
{
  const uint64_t ai_table_id = ctdef.table_param_.get_data_table().get_table_id();
  const bool ai_table = is_ai_column_table(ai_table_id);
  LOG_INFO("ai column datum hook", "table_id", ai_table_id, "count", count,
           "col_ids", ctdef.column_ids_.count(), "is_ai_table", ai_table,
           "tx_id", tx_id, "mode", static_cast<int>(mode));
  if (OB_ISNULL(datums) || !ai_table) {
    return;
  }
  const int64_t use_count = (count > 0) ? count : ctdef.column_ids_.count();
  if (use_count <= 0) {
    return;
  }
  const bool update_derived_insert = (AI_COLUMN_DML_INSERT == mode)
      && (ctdef.is_update_pk_ || ctdef.is_insert_up_);
  LOG_INFO("ai column datum hook detail", "mode", static_cast<int>(mode),
           "update_derived", update_derived_insert);
  ObAiColumnRowObserver observer;
  if (common::OB_SUCCESS == observer.init(&ctdef, tx_id, mode) && observer.need_observe()) {
    observer.set_single_row_mode(true);
    observer.set_force_invalidate(update_derived_insert);
    observer.observe(datums, use_count, false);
  }
}

// The update path hands over the old and the new row of one updated row; both
// use ins_ctdef.column_ids_ layout.  When a dependency or the primary key
// changed, the AI columns of the new row are invalidated in place (the caller
// writes this very row) and a task is registered for it.
inline bool ai_datum_equal(const ObObjMeta &meta, const common::ObDatum &lhs,
                           const common::ObDatum &rhs)
{
  if (lhs.is_null() || rhs.is_null()) {
    return lhs.is_null() && rhs.is_null();
  }
  const ObObjType type = meta.get_type();
  if (ob_is_integer_type(type) || ob_is_bit_tc(type) || ob_is_temporal_type(type)) {
    return lhs.get_int() == rhs.get_int();
  } else if (ob_is_unsigned_type(type)) {
    return lhs.get_uint64() == rhs.get_uint64();
  } else {
    const common::ObString ls = lhs.get_string();
    const common::ObString rs = rhs.get_string();
    return ls.length() == rs.length()
        && (0 == ls.length() || 0 == MEMCMP(ls.ptr(), rs.ptr(), ls.length()));
  }
}

inline void observe_ai_column_update_row(const ObDASDMLBaseCtDef &ctdef,
                                         const uint64_t tx_id,
                                         const ObChunkDatumStore::StoredRow &old_row,
                                         ObChunkDatumStore::StoredRow &new_row)
{
  const uint64_t real_table_id = ctdef.table_param_.get_data_table().get_table_id();
  if (!is_ai_column_table(real_table_id)
      || old_row.cnt_ != new_row.cnt_
      || old_row.cnt_ < ctdef.column_ids_.count()) {
    return;
  }
  share::ai_column::ObAiColumnTableMeta meta;
  bool found = false;
  if (OB_SUCCESS != share::ai_column::ObAiColumnMetaCache::get(real_table_id, meta, found)
      || !found) {
    return;
  }
  bool invalidated = false;
  for (int64_t i = 0; i < meta.column_count_ && !invalidated; ++i) {
    const share::ai_column::ObAiColumnMeta &col = meta.columns_[i];
    for (int64_t d = 0; d < col.dep_count_ && !invalidated; ++d) {
      for (int64_t idx = 0; idx < ctdef.column_ids_.count() && !invalidated; ++idx) {
        if (ctdef.column_ids_.at(idx) != col.dep_ids_[d]) {
          continue;
        }
        const ObObjMeta cm = (idx < ctdef.column_types_.count())
            ? ctdef.column_types_.at(idx) : ObObjMeta();
        invalidated = !ai_datum_equal(cm, old_row.cells()[idx], new_row.cells()[idx]);
      }
    }
    for (int64_t pk = 0; pk < meta.pk_count_ && !invalidated; ++pk) {
      for (int64_t idx = 0; idx < ctdef.column_ids_.count() && !invalidated; ++idx) {
        if (ctdef.column_ids_.at(idx) != meta.pk_ids_[pk]) {
          continue;
        }
        const ObObjMeta cm = (idx < ctdef.column_types_.count())
            ? ctdef.column_types_.at(idx) : ObObjMeta();
        invalidated = !ai_datum_equal(cm, old_row.cells()[idx], new_row.cells()[idx]);
      }
    }
  }
  if (!invalidated) {
    return;
  }
  LOG_INFO("ai column update invalidated", "table_id", real_table_id, K(tx_id));
  for (int64_t i = 0; i < meta.column_count_; ++i) {
    for (int64_t idx = 0; idx < ctdef.column_ids_.count(); ++idx) {
      if (ctdef.column_ids_.at(idx) == meta.columns_[i].column_id_) {
        new_row.cells()[idx].set_null();
      }
    }
  }
  observe_ai_column_datums(ctdef, tx_id, AI_COLUMN_DML_INSERT, new_row.cells(),
                           ctdef.column_ids_.count());
}

// The AI columns of a table have to be written by an update even when the
// statement does not assign them: the executor invalidates them in place, and
// the storage layer only persists columns listed in the update write set.
inline int append_ai_column_ids(const ObDASDMLBaseCtDef &ctdef,
                                common::ObSEArray<uint64_t, 16> &write_set)
{
  int ret = common::OB_SUCCESS;
  const uint64_t real_table_id = ctdef.table_param_.get_data_table().get_table_id();
  share::ai_column::ObAiColumnTableMeta meta;
  bool found = false;
  if (OB_FAIL(write_set.assign(
          static_cast<const ObDASUpdCtDef &>(ctdef).updated_column_ids_))) {
  }
  if (OB_FAIL(ret)) {
  } else if (!is_ai_column_table(real_table_id)) {
    // nothing to add
  } else if (OB_FAIL(share::ai_column::ObAiColumnMetaCache::get(real_table_id, meta, found))) {
  } else if (!found) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < meta.column_count_; ++i) {
      const uint64_t column_id = meta.columns_[i].column_id_;
      bool exists = false;
      for (int64_t j = 0; !exists && j < write_set.count(); ++j) {
        exists = (write_set.at(j) == column_id);
      }
      if (!exists && OB_FAIL(write_set.push_back(column_id))) {
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_DAS_OB_DAS_AI_COLUMN_OBSERVER_H_
