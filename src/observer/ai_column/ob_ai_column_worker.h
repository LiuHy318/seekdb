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

#ifndef OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_
#define OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_

#include <pthread.h>
#include <unistd.h>
#include "lib/ob_define.h"
#include "lib/atomic/ob_atomic.h"
#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_array.h"
#include "lib/time/ob_time_utility.h"
#include "share/ai_column/ob_ai_column_task_registry.h"
#include "share/ob_define.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/utility/ob_macro_utils.h"
#include "share/ob_server_struct.h"

namespace oceanbase
{
namespace observer
{
// The AI column worker drains oai_column_task_registry by running, for every
// (table, rowkey) group, one single-statement update that re-evaluates the AI
// column expression.  Because the statement is issued from a brand new
// transaction through the inner sql proxy, results are only visible after the
// producing transaction committed, and a rolled back producer can never trigger
// a model request (the probe predicate `ai_col is null` never matches).
class ObAiColumnWorker
{
public:
  static ObAiColumnWorker &get_instance();
  int start();
  void stop();
  bool is_started() const { return started_; }
private:
  ObAiColumnWorker() : started_(false), stop_(false), thread_(0) {}
  ~ObAiColumnWorker() { stop(); }
  static void *run_thread(void *arg);
  void run_loop();
  void process_once();
  int execute_group(const common::ObIArray<share::ai_column::ObAiColumnTaskEntry> &group,
                    const int64_t now);
  static int resolve_database_name(const uint64_t database_id, char *buf, const int64_t buf_len);
  static int resolve_database_name_by_table(const uint64_t table_id, char *buf,
                                            const int64_t buf_len);
  static bool same_group(const share::ai_column::ObAiColumnTaskEntry &lhs,
                         const share::ai_column::ObAiColumnTaskEntry &rhs);
private:
  bool started_;
  bool stop_;
  pthread_t thread_;
};

inline ObAiColumnWorker &ObAiColumnWorker::get_instance()
{
  static ObAiColumnWorker s_instance;
  return s_instance;
}

inline bool ObAiColumnWorker::same_group(
    const share::ai_column::ObAiColumnTaskEntry &lhs,
    const share::ai_column::ObAiColumnTaskEntry &rhs)
{
  return lhs.table_id_ == rhs.table_id_
      && 0 == strncmp(lhs.rowkey_pred_, rhs.rowkey_pred_,
                      share::ai_column::AI_TASK_ROWKEY_LEN - 1)
      && 0 == strncmp(lhs.request_id_, rhs.request_id_,
                      share::ai_column::AI_TASK_REQUEST_LEN - 1);
}

inline int ObAiColumnWorker::start()
{
  int ret = common::OB_SUCCESS;
  if (!started_) {
    stop_ = false;
    const int err = pthread_create(&thread_, NULL, ObAiColumnWorker::run_thread, this);
    if (0 != err) {
      ret = common::OB_ERR_SYS;
      SERVER_LOG(WARN, "failed to create ai column worker thread", K(ret), K(err));
    } else {
      started_ = true;
      SERVER_LOG(INFO, "ai column worker started");
    }
  }
  return ret;
}

inline void ObAiColumnWorker::stop()
{
  if (started_) {
    ATOMIC_STORE(&stop_, true);
    // the loop checks the flag every 50ms; do not join, the worker only touches
    // the in-memory registry and the sql proxy
    started_ = false;
  }
}

inline void *ObAiColumnWorker::run_thread(void *arg)
{
  ObAiColumnWorker *worker = static_cast<ObAiColumnWorker *>(arg);
  if (OB_NOT_NULL(worker)) {
    worker->run_loop();
  }
  return NULL;
}

inline void ObAiColumnWorker::run_loop()
{
  while (!ATOMIC_LOAD(&stop_)) {
    process_once();
    usleep(50 * 1000L);
  }
}

inline void ObAiColumnWorker::process_once()
{
  int ret = common::OB_SUCCESS;
  if (OB_ISNULL(GCTX.sql_proxy_) || !GCTX.sql_proxy_->is_inited()) {
    // server is not ready yet
  } else {
    common::ObArray<share::ai_column::ObAiColumnTaskEntry> tasks;
    const int64_t now = common::ObTimeUtility::current_time();
    if (OB_FAIL(share::ai_column::ObAiColumnTaskRegistry::claim_runnable(now, tasks, 16))) {
      SERVER_LOG(WARN, "failed to claim ai column tasks", K(ret));
    } else {
      int64_t i = 0;
      while (common::OB_SUCCESS == ret && i < tasks.count()) {
        common::ObArray<share::ai_column::ObAiColumnTaskEntry> group;
        const share::ai_column::ObAiColumnTaskEntry &head = tasks.at(i);
        for (int64_t j = i; common::OB_SUCCESS == ret && j < tasks.count(); ++j) {
          if (same_group(head, tasks.at(j))) {
            if (OB_FAIL(group.push_back(tasks.at(j)))) {
              SERVER_LOG(WARN, "failed to push group member", K(ret));
            }
          }
        }
        if (common::OB_SUCCESS == ret) {
          if (OB_FAIL(execute_group(group, now))) {
            SERVER_LOG(WARN, "failed to execute ai column group", K(ret));
          }
        }
        // advance past the members handled above
        int64_t next = i + 1;
        while (next < tasks.count() && same_group(head, tasks.at(next))) {
          ++next;
        }
        i = next;
      }
    }
  }
}

inline int ObAiColumnWorker::resolve_database_name_by_table(const uint64_t table_id,
                                                           char *buf,
                                                           const int64_t buf_len)
{
  int ret = common::OB_SUCCESS;
  static uint64_t s_ids[16];
  static char s_names[16][share::ai_column::AI_TASK_NAME_LEN];
  int64_t free_slot = -1;
  buf[0] = '\0';
  for (int64_t i = 0; i < 16; ++i) {
    if (s_ids[i] == table_id) {
      snprintf(buf, buf_len, "%s", s_names[i]);
      return ret;
    } else if (0 == s_ids[i] && free_slot < 0) {
      free_slot = i;
    }
  }
  common::ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("select d.database_name from oceanbase.__all_table t "
                             "join oceanbase.__all_database d on d.database_id = t.database_id "
                             "where t.table_id = %lu limit 1", table_id))) {
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = common::OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else {
        common::ObString name;
        if (OB_FAIL(result->get_varchar("database_name", name))) {
        } else {
          int64_t n = MIN(name.length(), buf_len - 1);
          MEMCPY(buf, name.ptr(), n);
          buf[n] = '\0';
          if (free_slot >= 0) {
            s_ids[free_slot] = table_id;
            snprintf(s_names[free_slot], share::ai_column::AI_TASK_NAME_LEN, "%s", buf);
          }
        }
      }
    }
  }
  return ret;
}

inline int ObAiColumnWorker::resolve_database_name(const uint64_t database_id, char *buf,
                                                    const int64_t buf_len)
{
  int ret = common::OB_SUCCESS;
  // tiny cache: schema objects rarely change inside one test run
  static uint64_t s_ids[16];
  static char s_names[16][share::ai_column::AI_TASK_NAME_LEN];
  int64_t free_slot = -1;
  buf[0] = '\0';
  for (int64_t i = 0; i < 16; ++i) {
    if (s_ids[i] == database_id) {
      snprintf(buf, buf_len, "%s", s_names[i]);
      return ret;
    } else if (0 == s_ids[i] && free_slot < 0) {
      free_slot = i;
    }
  }
  common::ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("select database_name from oceanbase.__all_database "
                             "where database_id = %lu limit 1", database_id))) {
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = common::OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else {
        common::ObString name;
        if (OB_FAIL(result->get_varchar("database_name", name))) {
        } else {
          int64_t n = MIN(name.length(), buf_len - 1);
          MEMCPY(buf, name.ptr(), n);
          buf[n] = '\0';
          if (free_slot >= 0) {
            s_ids[free_slot] = database_id;
            snprintf(s_names[free_slot], share::ai_column::AI_TASK_NAME_LEN, "%s", buf);
          }
        }
      }
    }
  }
  return ret;
}

inline int ObAiColumnWorker::execute_group(
    const common::ObIArray<share::ai_column::ObAiColumnTaskEntry> &group,
    const int64_t now)
{
  int ret = common::OB_SUCCESS;
  if (0 == group.count()) {
    // nothing to do
  } else {
    common::ObSqlString sql;
    const share::ai_column::ObAiColumnTaskEntry &head = group.at(0);
    char db_name[share::ai_column::AI_TASK_NAME_LEN];
    db_name[0] = '\0';
    if (OB_FAIL(resolve_database_name_by_table(head.table_id_, db_name, sizeof(db_name)))) {
      SERVER_LOG(WARN, "failed to resolve database name", K(ret),
                 "table_id", head.table_id_);
      for (int64_t i = 0; i < group.count(); ++i) {
        share::ai_column::ObAiColumnTaskRegistry::mark_error(
            group.at(i).task_id_, now, "resolve database name failed");
      }
      ret = common::OB_SUCCESS;
    } else if (OB_FAIL(sql.append_fmt("update `%.*s`.`%.*s` set ",
                               static_cast<int>(strlen(db_name)), db_name,
                               static_cast<int>(strlen(head.table_name_)),
                               head.table_name_))) {
      SERVER_LOG(WARN, "failed to build update sql", K(ret));
    }
    for (int64_t i = 0; common::OB_SUCCESS == ret && i < group.count(); ++i) {
      const share::ai_column::ObAiColumnTaskEntry &e = group.at(i);
      if (OB_FAIL(sql.append_fmt("%s`%.*s` = %s",
                                 (0 == i) ? "" : ", ",
                                 static_cast<int>(strlen(e.column_name_)), e.column_name_,
                                 e.expression_))) {
        SERVER_LOG(WARN, "failed to append assignment", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append_fmt(" where (%s)", head.rowkey_pred_))) {
      SERVER_LOG(WARN, "failed to append predicate", K(ret));
    }
    for (int64_t i = 0; common::OB_SUCCESS == ret && i < group.count(); ++i) {
      const share::ai_column::ObAiColumnTaskEntry &e = group.at(i);
      if (OB_FAIL(sql.append_fmt(" and `%.*s` is null",
                                 static_cast<int>(strlen(e.column_name_)), e.column_name_))) {
        SERVER_LOG(WARN, "failed to append null guard", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else {
      int64_t affected_rows = 0;
      LOG_INFO("ai column worker sql", "sql", sql.ptr());
      ret = GCTX.sql_proxy_->write(sql.ptr(), affected_rows);
      LOG_INFO("ai column worker sql done", K(ret), K(affected_rows));
      if (common::OB_SUCCESS != ret) {
        SERVER_LOG(WARN, "ai column worker update failed", K(ret), K(affected_rows), "sql", sql.ptr());
        char err_msg[share::ai_column::AI_TASK_MSG_LEN];
        MEMSET(err_msg, 0, sizeof(err_msg));
        snprintf(err_msg, sizeof(err_msg), "%s", ob_strerror(ret));
        for (int64_t i = 0; i < group.count(); ++i) {
          share::ai_column::ObAiColumnTaskRegistry::mark_error(group.at(i).task_id_, now, err_msg);
        }
        ret = common::OB_SUCCESS;
      } else if (affected_rows > 0) {
        for (int64_t i = 0; i < group.count(); ++i) {
          share::ai_column::ObAiColumnTaskRegistry::mark_done(group.at(i).task_id_);
        }
      } else {
        // the row is either not committed yet, or it has already been computed
        for (int64_t i = 0; i < group.count(); ++i) {
          share::ai_column::ObAiColumnTaskRegistry::mark_zero_hit(group.at(i).task_id_, now);
        }
      }
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_
