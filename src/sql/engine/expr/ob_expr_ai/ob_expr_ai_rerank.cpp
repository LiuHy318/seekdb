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
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc,
                    T_FUN_SYS_AI_RERANK,
                    N_AI_RERANK,
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL,
                    NOT_ROW_DIMENSION)
{
}

ObExprAIRerank::~ObExprAIRerank()
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid param type", K(ret), K(types_stack[MODEL_IDX]));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid param type", K(ret), K(types_stack[QUERY_IDX]));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid param type", K(ret), K(types_stack[DOC_KEY_IDX]));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("model id or query or documents is null", K(ret));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObJsonArray *result_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    int64_t batch_size = 20; // max batch size for rerank
    bool is_null_result = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
          j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("j_base is null", K(ret));
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("document_array is not array", K(ret));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("document_array is null", K(ret));
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("document_array is empty", K(ret));
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("doc_node is null", K(ret));
        } else if (OB_NOT_NULL(arg_doc_key) && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("doc_node is not object", K(ret));
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (OB_ISNULL(arg_doc_key) && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("doc_node is not string", K(ret));
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("AI endpoint resolver is unavailable", K(ret));
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    }

    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("info is null", K(ret));
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_NOT_NULL(arg_doc_key)) {
      ObString doc_key = arg_doc_key->get_string();
      if (OB_FAIL(eval_ai_rerank_with_doc_key(expr, ctx, temp_allocator, model_id,
                  query, document_array, doc_key, *info, *endpoint_info, res))) {
      }
    } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info, header_array))) {
    } else {
      ObJsonArray *all_result_array = nullptr;
      ObJsonArray *sorted_result_array = nullptr;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, all_result_array))) {
      } else {
        ObJsonArray *batch_document_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        const int64_t document_count = static_cast<int64_t>(document_array->element_count());
        for (int64_t start_idx = 0; OB_SUCC(ret) && start_idx < document_count; start_idx += batch_size) {
          int64_t end_idx = start_idx + batch_size;
          if (end_idx > document_count) {
            end_idx = document_count;
          }
          batch_document_array = nullptr;
          batch_result_array = nullptr;
          // One request carries at most batch_size documents.
          if (OB_FAIL(construct_batch_document_array(temp_allocator, document_array, start_idx,
                                                    end_idx, batch_document_array))) {
          } else if (OB_FAIL(inner_eval_ai_rerank(temp_allocator, *info, *endpoint_info, header_array,
                                                  query, batch_document_array, batch_result_array))) {
          } else if (OB_FAIL(validate_rerank_result(batch_result_array, end_idx - start_idx))) {
            LOG_WARN("invalid rerank result", K(ret), K(start_idx), K(end_idx));
          } else if (OB_FAIL(batch_result_add_base(temp_allocator, batch_result_array, start_idx))) {
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(batch_result_array->element_count()); i++) {
              if (OB_FAIL(all_result_array->append(batch_result_array->get_value(i)))) {
                LOG_WARN("failed to append rerank result", K(ret), K(i));
              }
            }
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(sort_rerank_result(temp_allocator, all_result_array, sorted_result_array))) {
        LOG_WARN("failed to sort rerank result", K(ret));
      } else {
        result_array = sorted_result_array;
      }
      if (OB_SUCC(ret)) {
        ObString raw_str;
        if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank_with_doc_key(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator,
                                                ObString &model_id, ObString &query, ObJsonArray *document_array,
                                                ObString &doc_key, const ObAIFuncExprInfo &info,
                                                const share::ObAiModelEndpointInfo &endpoint_info, ObDatum &res)
{
  INIT_SUCC(ret);
  UNUSED(model_id);
  ObJsonArray *doc_array = nullptr;
  ObJsonArray *all_result_array = nullptr;
  ObJsonArray *sorted_result_array = nullptr;
  ObJsonArray *sorted_document_array = nullptr;
  ObAIFuncModel model(allocator, info, endpoint_info);
  const int64_t batch_size = 20; // max batch size for rerank
  if (OB_FAIL(get_doc_array_from_documents_array_with_key(allocator, document_array, doc_key, doc_array))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, all_result_array))) {
  } else {
    const int64_t doc_count = static_cast<int64_t>(doc_array->element_count());
    for (int64_t start_idx = 0; OB_SUCC(ret) && start_idx < doc_count; start_idx += batch_size) {
      int64_t end_idx = start_idx + batch_size;
      if (end_idx > doc_count) {
        end_idx = doc_count;
      }
      ObJsonArray *batch_document_array = nullptr;
      ObJsonArray *batch_result_array = nullptr;
      // The documents are reranked batch by batch, and the index of every batch
      // is shifted by the batch base so that the index is a global one.
      if (OB_FAIL(construct_batch_document_array(allocator, doc_array, start_idx, end_idx, batch_document_array))) {
      } else if (OB_FAIL(model.call_rerank(query, batch_document_array, batch_result_array))) {
        LOG_WARN("failed to call rerank model", K(ret), K(start_idx), K(end_idx));
      } else if (OB_FAIL(validate_rerank_result(batch_result_array, end_idx - start_idx))) {
        LOG_WARN("invalid rerank result", K(ret), K(start_idx), K(end_idx));
      } else if (OB_FAIL(batch_result_add_base(allocator, batch_result_array, start_idx))) {
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(batch_result_array->element_count()); i++) {
          if (OB_FAIL(all_result_array->append(batch_result_array->get_value(i)))) {
            LOG_WARN("failed to append rerank result", K(ret), K(i));
          }
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sort_rerank_result(allocator, all_result_array, sorted_result_array))) {
    LOG_WARN("failed to sort rerank result", K(ret));
  } else if (OB_FAIL(attach_score_to_documents(allocator, document_array, sorted_result_array,
                                               sorted_document_array))) {
    LOG_WARN("failed to attach score to documents", K(ret));
  } else {
    ObString raw_str;
    if (OB_FAIL(sorted_document_array->get_raw_binary(raw_str, &allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("document_object_array is null", K(ret));
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("doc_key is empty", K(ret));
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("doc_node is null", K(ret));
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("doc_node is not object", K(ret));
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("doc_value_node is null", K(ret));
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("doc_value_node is not string", K(ret));
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator,
                                          const ObAIFuncExprInfo &info,
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array,
                                          ObString &query,
                                          ObJsonArray *document_array,
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("response is not array", K(ret));
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("res is null", K(ret));
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::batch_result_add_base(ObIAllocator &allocator, ObJsonArray *array, int64_t start_idx)
{
  INIT_SUCC(ret);
  int64_t count = array->element_count();
  ObString index_key(INDEX_KEY);
  for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
    ObJsonObject *obj = static_cast<ObJsonObject *>(array->get_value(i));
    if (OB_ISNULL(obj)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("obj is null", K(ret));
    } else {
      ObJsonInt *int_obj = static_cast<ObJsonInt *>(obj->get_value(index_key));
      if (OB_ISNULL(int_obj)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("int_obj is null", K(ret));
      } else {
        int64_t id = int_obj->value() + start_idx;
        int_obj->set_value(id);
      }
    }
  }
  return ret;
}

int ObExprAIRerank::validate_rerank_result(ObJsonArray *result_array, int64_t expected_count)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(result_array)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("result array is null", K(ret));
  } else if (static_cast<int64_t>(result_array->element_count()) != expected_count) {
    ret = OB_INVALID_DATA;
    LOG_WARN("the result count mismatches the document count", K(ret),
             K(result_array->element_count()), K(expected_count));
    FORWARD_USER_ERROR(ret, "ai_rerank, the count of results mismatches the count of documents");
  } else {
    ObString index_key(INDEX_KEY);
    ObString score_key(SCORE_KEY);
    ObArray<bool> visited;
    for (int64_t i = 0; OB_SUCC(ret) && i < expected_count; i++) {
      if (OB_FAIL(visited.push_back(false))) {
        LOG_WARN("failed to push visited flag", K(ret), K(i));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < expected_count; i++) {
      ObJsonNode *result_node = result_array->get_value(i);
      ObJsonObject *result_obj = nullptr;
      ObJsonNode *index_node = nullptr;
      ObJsonNode *score_node = nullptr;
      double index_value = 0.0;
      double score_value = 0.0;
      if (OB_ISNULL(result_node) || result_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("the rerank result is not a json object", K(ret), K(i));
      } else if (OB_ISNULL(result_obj = static_cast<ObJsonObject *>(result_node))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result object is null", K(ret), K(i));
      } else if (OB_ISNULL(index_node = result_obj->get_value(index_key)) ||
                 OB_ISNULL(score_node = result_obj->get_value(score_key))) {
        ret = OB_INVALID_DATA;
        LOG_WARN("the rerank result misses index or score", K(ret), K(i));
        FORWARD_USER_ERROR(ret, "ai_rerank, the rerank result misses index or score");
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_number_value(index_node, index_value))) {
        LOG_WARN("index of the rerank result is not a number", K(ret), K(i));
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_number_value(score_node, score_value))) {
        LOG_WARN("score of the rerank result is not a number", K(ret), K(i));
      } else if (index_value < 0 || index_value >= static_cast<double>(expected_count) ||
                 index_value != static_cast<double>(static_cast<int64_t>(index_value))) {
        ret = OB_INVALID_DATA;
        LOG_WARN("the index of the rerank result is out of range", K(ret), K(i), K(index_value));
        FORWARD_USER_ERROR(ret, "ai_rerank, the index of the rerank result is out of range");
      } else if (visited.at(static_cast<int64_t>(index_value))) {
        ret = OB_INVALID_DATA;
        LOG_WARN("duplicated index in the rerank result", K(ret), K(i), K(index_value));
        FORWARD_USER_ERROR(ret, "ai_rerank, duplicated index in the rerank result");
      } else {
        visited.at(static_cast<int64_t>(index_value)) = true;
      }
    }
  }
  return ret;
}

int ObExprAIRerank::sort_rerank_result(ObIAllocator &allocator, ObJsonArray *result_array,
                                       ObJsonArray *&sorted_array)
{
  INIT_SUCC(ret);
  struct RerankItem
  {
    double score_;
    int64_t index_;
    ObJsonObject *obj_;
    int64_t to_string(char *buf, const int64_t buf_len) const
    {
      int64_t pos = 0;
      databuff_printf(buf, buf_len, pos, "{score: %lf, index: %ld}", score_, index_);
      return pos;
    }
  };
  ObJsonArray *sorted_result_array = nullptr;
  ObString index_key(INDEX_KEY);
  ObString score_key(SCORE_KEY);
  if (OB_ISNULL(result_array)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("result array is null", K(ret));
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_result_array))) {
  } else {
    ObArray<RerankItem> items;
    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(result_array->element_count()); i++) {
      ObJsonNode *result_node = result_array->get_value(i);
      ObJsonObject *result_obj = nullptr;
      double score_value = 0.0;
      double index_value = 0.0;
      RerankItem item;
      if (OB_ISNULL(result_node) || result_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("the rerank result is not a json object", K(ret), K(i));
      } else if (OB_ISNULL(result_obj = static_cast<ObJsonObject *>(result_node))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result object is null", K(ret), K(i));
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_number_value(result_obj->get_value(score_key), score_value))) {
        LOG_WARN("failed to get score", K(ret), K(i));
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_number_value(result_obj->get_value(index_key), index_value))) {
        LOG_WARN("failed to get index", K(ret), K(i));
      } else {
        item.score_ = score_value;
        item.index_ = static_cast<int64_t>(index_value);
        item.obj_ = result_obj;
        if (OB_FAIL(items.push_back(item))) {
          LOG_WARN("failed to push rerank item", K(ret), K(i));
        }
      }
    }
    // Insertion sort, descending by score and ascending by index, so that the
    // documents with the same score keep their original order.
    for (int64_t i = 1; OB_SUCC(ret) && i < static_cast<int64_t>(items.count()); i++) {
      RerankItem cur = items.at(i);
      int64_t j = i - 1;
      while (j >= 0 && (items.at(j).score_ < cur.score_ ||
                        (items.at(j).score_ == cur.score_ && items.at(j).index_ > cur.index_))) {
        items.at(j + 1) = items.at(j);
        j--;
      }
      items.at(j + 1) = cur;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(items.count()); i++) {
      if (OB_FAIL(sorted_result_array->append(items.at(i).obj_))) {
        LOG_WARN("failed to append sorted result", K(ret), K(i));
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_array = sorted_result_array;
  }
  return ret;
}

int ObExprAIRerank::attach_score_to_documents(ObIAllocator &allocator, ObJsonArray *document_array,
                                              ObJsonArray *sorted_result_array, ObJsonArray *&sorted_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *document_result_array = nullptr;
  ObString index_key(INDEX_KEY);
  ObString score_key(SCORE_KEY);
  if (OB_ISNULL(document_array) || OB_ISNULL(sorted_result_array)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid argument", K(ret));
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, document_result_array))) {
  } else {
    const int64_t document_count = static_cast<int64_t>(document_array->element_count());
    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(sorted_result_array->element_count()); i++) {
      ObJsonObject *result_obj = static_cast<ObJsonObject *>(sorted_result_array->get_value(i));
      ObJsonNode *index_node = nullptr;
      ObJsonNode *score_node = nullptr;
      double index_value = 0.0;
      int64_t document_idx = 0;
      ObJsonNode *document_node = nullptr;
      ObJsonNode *new_document_node = nullptr;
      if (OB_ISNULL(result_obj)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result object is null", K(ret), K(i));
      } else if (OB_ISNULL(index_node = result_obj->get_value(index_key))) {
        ret = OB_INVALID_DATA;
        LOG_WARN("index is missed", K(ret), K(i));
      } else if (OB_ISNULL(score_node = result_obj->get_value(score_key))) {
        ret = OB_INVALID_DATA;
        LOG_WARN("score is missed", K(ret), K(i));
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_number_value(index_node, index_value))) {
        LOG_WARN("index is not a number", K(ret), K(i));
      } else if (FALSE_IT(document_idx = static_cast<int64_t>(index_value))) {
      } else if (document_idx < 0 || document_idx >= document_count) {
        ret = OB_INVALID_DATA;
        LOG_WARN("document index is out of range", K(ret), K(document_idx), K(document_count));
        FORWARD_USER_ERROR(ret, "ai_rerank, the index of the rerank result is out of range");
      } else if (OB_ISNULL(document_node = document_array->get_value(document_idx))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("document is null", K(ret), K(document_idx));
      } else if (OB_ISNULL(new_document_node = document_node->clone(&allocator, true))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to clone document", K(ret), K(document_idx));
      } else if (OB_FAIL(static_cast<ObJsonObject *>(new_document_node)->add(MODEL_SCORE_KEY,
                     score_node->clone(&allocator, true)))) {
        LOG_WARN("failed to add model score", K(ret), K(i));
      } else if (OB_FAIL(document_result_array->append(new_document_node))) {
        LOG_WARN("failed to append document", K(ret), K(i));
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_document_array = document_result_array;
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx,
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
  //   bool got_data = false;
  //   ObObj const_data;
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
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
