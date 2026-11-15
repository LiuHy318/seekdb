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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                        T_FUN_SYS_AI_EMBED,
                        N_AI_EMBED,
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL,
                        NOT_ROW_DIMENSION)
{
}

ObExprAIEmbed::~ObExprAIEmbed()
{
}

// TEXT and LONGTEXT inputs are supported, they are read as LOB data.
static bool is_ai_embed_text_type(const ObObjType type)
{
  return ObTinyTextType == type || ObTextType == type ||
         ObMediumTextType == type || ObLongTextType == type;
}

static bool is_same_string(const ObString &lhs, const ObString &rhs)
{
  return lhs.length() == rhs.length() &&
         (0 == lhs.length() || 0 == MEMCMP(lhs.ptr(), rhs.ptr(), lhs.length()));
}

// Read the optional dimension argument of one row, 0 means the argument is not
// specified (or is NULL), in which case the model default dimension is used.
static int get_ai_embed_dimension(const ObExpr &expr, const ObDatumVector &dim_vec,
                                  const int64_t idx, int64_t &dim)
{
  INIT_SUCC(ret);
  dim = 0;
  if (expr.arg_cnt_ < 3) {
    dim = 0;
  } else if (OB_ISNULL(dim_vec.at(idx)) || dim_vec.at(idx)->is_null()) {
    dim = 0;
  } else if (FALSE_IT(dim = dim_vec.at(idx)->get_int())) {
  } else if (dim <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("dimension parameter must be a positive integer", K(ret), K(dim));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
  }
  return ret;
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (ob_is_string_tc(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
      types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else if (is_ai_embed_text_type(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else {
      ret = OB_ERR_INVALID_TYPE_FOR_OP;
      LOG_WARN("invalid content type", K(ret), K(types_stack[CONTENT_IDX].get_type()));
      LOG_USER_ERROR(OB_ERR_INVALID_TYPE_FOR_OP, "ai_embed, invalid input type");
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("dimension parameter must be an integer, not a decimal or float", K(ret), K(types_stack[DIM_IDX].get_type()));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    if (OB_SUCC(ret)) {
      type.set_varchar();
      type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      type.set_collation_level(CS_LEVEL_COERCIBLE);
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
    LOG_WARN("evaluate parameters failed", K(ret));
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    // A NULL input propagates a NULL result, the row never reaches the model
    // service, which keeps a batched scan and a row-by-row scan consistent.
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    if (OB_FAIL(ObTextStringHelper::get_string(ctx.exec_ctx_, expr, temp_allocator,
                                               ObExprAIEmbed::CONTENT_IDX, arg_content, content))) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("model id or input is empty", K(ret));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim) && !arg_dim->is_null()) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("dimension parameter must be a positive integer", K(ret), K(dim));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("AI endpoint resolver is unavailable", K(ret));
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t size)
{
  INIT_SUCC(ret);
  ObDatum *res = expr.locate_batch_datums(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObDatumVector model_vec;
  ObDatumVector content_vec;
  ObDatumVector dim_vec;
  if (OB_ISNULL(res)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("res datums is null", K(ret));
  } else if (OB_FAIL(expr.eval_batch_param_value(ctx, skip, size, model_vec, content_vec, dim_vec))) {
    LOG_WARN("failed to evaluate batch parameters", K(ret));
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));

    // Rows with a NULL input propagate NULL, they do not take part in any request.
    for (int64_t i = 0; OB_SUCC(ret) && i < size; i++) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      } else if (model_vec.at(i)->is_null() || content_vec.at(i)->is_null()) {
        res[i].set_null();
        eval_flags.set(i);
      }
    }

    int64_t row_idx = 0;
    while (OB_SUCC(ret) && row_idx < size) {
      if (skip.at(row_idx) || eval_flags.at(row_idx)) {
        row_idx++;
        continue;
      }
      ObString chunk_model_id = model_vec.at(row_idx)->get_string();
      int64_t chunk_dim = 0;
      if (OB_FAIL(get_ai_embed_dimension(expr, dim_vec, row_idx, chunk_dim))) {
        LOG_WARN("failed to get dimension", K(ret), K(row_idx));
      } else if (chunk_model_id.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("model id is empty", K(ret), K(row_idx));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is empty");
      }
      ObArray<ObString> contents;
      ObArray<int64_t> row_indexes;
      int64_t next_idx = row_idx;
      // Gather the rows embedded by one request: same model key, same dimension
      // and no more than MAX_EMBED_BATCH_SIZE inputs.
      while (OB_SUCC(ret) && next_idx < size &&
             static_cast<int64_t>(row_indexes.count()) < MAX_EMBED_BATCH_SIZE) {
        if (skip.at(next_idx) || eval_flags.at(next_idx)) {
          next_idx++;
          continue;
        }
        ObString row_model_id = model_vec.at(next_idx)->get_string();
        int64_t row_dim = 0;
        ObString content;
        if (OB_FAIL(get_ai_embed_dimension(expr, dim_vec, next_idx, row_dim))) {
          LOG_WARN("failed to get dimension", K(ret), K(next_idx));
        } else if (!is_same_string(row_model_id, chunk_model_id) || row_dim != chunk_dim) {
          break;
        } else if (OB_FAIL(ObTextStringHelper::get_string(ctx.exec_ctx_, expr, temp_allocator,
                                                          ObExprAIEmbed::CONTENT_IDX,
                                                          content_vec.at(next_idx), content))) {
          LOG_WARN("failed to read content", K(ret), K(next_idx));
        } else if (content.empty()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("content is empty", K(ret), K(next_idx));
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
        } else if (OB_FAIL(contents.push_back(content))) {
          LOG_WARN("failed to push content", K(ret));
        } else if (OB_FAIL(row_indexes.push_back(next_idx))) {
          LOG_WARN("failed to push row index", K(ret));
        } else {
          next_idx++;
        }
      }

      if (OB_FAIL(ret)) {
      } else if (contents.empty()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("no content is collected", K(ret), K(row_idx));
      } else {
        ObAIFuncExprInfo *info = nullptr;
        share::ObAiModelEndpointInfo resolved_endpoint;
        const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
        query::ObIAiEndpointResolver *endpoint_resolver =
            ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
        ObJsonObject *config = nullptr;
        ObJsonInt *dim_json = nullptr;
        ObArray<ObString> results;
        if (chunk_dim > 0) {
          if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
          } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, chunk_dim, dim_json))) {
          } else if (OB_FAIL(config->add("dimensions", dim_json))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, chunk_model_id, info))) {
        } else if (OB_ISNULL(endpoint_resolver)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("AI endpoint resolver is unavailable", K(ret));
        } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     chunk_model_id, temp_allocator, resolved_endpoint))) {
        } else {
          ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
          if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
            LOG_WARN("failed to call embedding model", K(ret), K(contents.count()));
          } else if (results.count() != contents.count()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("the result count mismatches the input count", K(ret),
                     K(results.count()), K(contents.count()));
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < results.count(); i++) {
            const int64_t batch_row_idx = row_indexes.at(i);
            ObTextStringDatumResult res_result(expr.datum_meta_.type_, &expr, &ctx, &res[batch_row_idx]);
            if (OB_FAIL(res_result.init_with_batch_idx(results.at(i).length(), batch_row_idx))) {
              LOG_WARN("failed to init text result", K(ret), K(batch_row_idx));
            } else if (OB_FAIL(res_result.append(results.at(i)))) {
              LOG_WARN("failed to append text result", K(ret), K(batch_row_idx));
            } else {
              res_result.set_result();
              eval_flags.set(batch_row_idx);
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        row_idx = next_idx;
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx,
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
