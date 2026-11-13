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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/ddl/ob_ddl_stmt.h"
#include "sql/ob_sql_context.h"
#include "sql/session/ob_basic_session_info.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{
int ObDDLStmt::get_first_stmt(ObString &first_stmt)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(ObStmt::get_first_stmt(first_stmt))) {
  } else if (OB_ISNULL(get_query_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObCharset::charset_convert(allocator_,
                                                first_stmt,
                                                get_query_ctx()->get_sql_stmt_coll_type(),
                                                ObCharset::get_system_collation(),
                                                first_stmt,
                                                ObCharset::REPLACE_UNKNOWN_CHARACTER_ON_SAME_CHARSET))) {
  }

  return ret;
}

int ObDDLStmt::get_parallelism_explicit(const ObBasicSessionInfo *session_info, bool &is_explicit) const
{
  int ret = OB_SUCCESS;
  bool enable_parallel_ddl = true;
  if (OB_ISNULL(session_info)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(session_info->get_enable_parallel_ddl(enable_parallel_ddl))) {
    // A statement whose parallelism can not be traced back to the session keeps
    // the historical behaviour: treat it as asked for, so nothing widens it.
    // The DDL must not fail over this: the flag only decides a degree of
    // parallelism, and the value it is set to here is the conservative one.
    LOG_WARN("get enable parallel ddl failed, treat the parallelism as explicit", K(ret));
    is_explicit = true;
    ret = OB_SUCCESS;
  } else {
    is_explicit = has_parallel_hint_ || !enable_parallel_ddl;
  }
  return ret;
}
}
}
