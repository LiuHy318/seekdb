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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{
namespace
{
// tokenize() is a deterministic pure function of the text, the parser, the parser properties and
// the output mode, and callers commonly evaluate the very same call again and again - a constant
// argument inside a reused plan, a client that repeats one statement. Every one of those calls used
// to pay for the whole parse. Remember the last few argument sets of this thread together with the
// raw JSON they produced, so a repeated call only has to compare the arguments and copy the bytes.
// An argument set that does not fit into an entry simply is not remembered, and a diagnostic that
// looks the same but is not identical in every byte is compared away - correctness never depends on
// a cache hit.
const int64_t TOKENIZE_MEMO_ENTRY_CNT = 2;

struct ObTokenizeMemoEntry
{
  static const int64_t MAX_TEXT_LEN = 1024;
  static const int64_t MAX_PARSER_NAME_LEN = 64;
  static const int64_t MAX_PROPERTIES_LEN = 1024;
  static const int64_t MAX_RESULT_LEN = 16384;

  bool valid_;
  uint64_t hash_;
  bool raw_arguments_;
  int64_t mode_;
  int64_t collation_;
  int64_t parser_name_len_;
  int64_t properties_len_;
  int64_t text_len_;
  int64_t result_len_;
  char parser_name_[MAX_PARSER_NAME_LEN];
  char properties_[MAX_PROPERTIES_LEN];
  char text_[MAX_TEXT_LEN];
  char result_[MAX_RESULT_LEN];
};

struct ObTokenizeMemo
{
  ObTokenizeMemoEntry entries_[TOKENIZE_MEMO_ENTRY_CNT];
  int64_t next_;
};

// Raw SQL names and normalized internal names use distinct memo identities:
// "space.1" is valid internally but must still be rejected as a public name.
// Zero initialized for every thread, no constructor needed.
thread_local ObTokenizeMemo TLS_TOKENIZE_MEMO;

void tokenize_memo_mix(uint64_t &hash, const char *str, const int64_t len)
{
  const unsigned char *ptr = reinterpret_cast<const unsigned char *>(str);
  int64_t i = 0;
  for (; i + 8 <= len; i += 8) {
    uint64_t word = 0;
    MEMCPY(&word, ptr + i, 8);
    hash = (hash ^ word) * 1099511628211ULL;
    hash ^= hash >> 29;
  }
  if (i < len) {
    uint64_t tail = 0;
    MEMCPY(&tail, ptr + i, len - i);
    hash = (hash ^ tail) * 1099511628211ULL;
  }
  hash ^= hash >> 32;
}

uint64_t tokenize_memo_hash(const common::ObString &parser_name,
                            const common::ObString &properties,
                            const common::ObString &text,
                            const int64_t mode,
                            const int64_t collation)
{
  uint64_t hash = 1469598103934665603ULL;
  hash = (hash ^ static_cast<uint64_t>(mode)) * 1099511628211ULL;
  hash = (hash ^ static_cast<uint64_t>(collation)) * 1099511628211ULL;
  tokenize_memo_mix(hash, parser_name.ptr(), parser_name.length());
  tokenize_memo_mix(hash, properties.ptr(), properties.length());
  tokenize_memo_mix(hash, text.ptr(), text.length());
  return hash;
}

bool tokenize_memo_fits(const common::ObString &parser_name,
                        const common::ObString &properties,
                        const common::ObString &text)
{
  return parser_name.length() <= ObTokenizeMemoEntry::MAX_PARSER_NAME_LEN
         && properties.length() <= ObTokenizeMemoEntry::MAX_PROPERTIES_LEN
         && text.length() <= ObTokenizeMemoEntry::MAX_TEXT_LEN;
}

bool tokenize_memo_same_bytes(const char *lhs,
                              const int64_t lhs_len,
                              const common::ObString &rhs)
{
  return lhs_len == rhs.length() && (0 == lhs_len || 0 == MEMCMP(lhs, rhs.ptr(), lhs_len));
}

bool tokenize_memo_lookup(const common::ObString &parser_name,
                          const common::ObString &properties,
                          const common::ObString &text,
                          const int64_t mode,
                          const int64_t collation,
                          const bool raw_arguments,
                          common::ObString &result)
{
  bool found = false;
  const uint64_t hash = tokenize_memo_hash(parser_name, properties, text, mode, collation);
  ObTokenizeMemoEntry *entries = TLS_TOKENIZE_MEMO.entries_;
  for (int64_t i = 0; !found && i < TOKENIZE_MEMO_ENTRY_CNT; ++i) {
    ObTokenizeMemoEntry &entry = entries[i];
    if (entry.valid_
        && entry.raw_arguments_ == raw_arguments
        && entry.hash_ == hash
        && entry.mode_ == mode
        && entry.collation_ == collation
        && tokenize_memo_same_bytes(entry.parser_name_, entry.parser_name_len_, parser_name)
        && tokenize_memo_same_bytes(entry.properties_, entry.properties_len_, properties)
        && tokenize_memo_same_bytes(entry.text_, entry.text_len_, text)) {
      result.assign_ptr(entry.result_, static_cast<int32_t>(entry.result_len_));
      found = true;
    }
  }
  return found;
}

void tokenize_memo_store(const common::ObString &parser_name,
                         const common::ObString &properties,
                         const common::ObString &text,
                         const int64_t mode,
                         const int64_t collation,
                         const bool raw_arguments,
                         const common::ObString &result)
{
  if (tokenize_memo_fits(parser_name, properties, text)
      && result.length() <= ObTokenizeMemoEntry::MAX_RESULT_LEN) {
    ObTokenizeMemo &memo = TLS_TOKENIZE_MEMO;
    ObTokenizeMemoEntry &entry = memo.entries_[memo.next_ % TOKENIZE_MEMO_ENTRY_CNT];
    memo.next_ = (memo.next_ + 1) % TOKENIZE_MEMO_ENTRY_CNT;
    entry.hash_ = tokenize_memo_hash(parser_name, properties, text, mode, collation);
    entry.raw_arguments_ = raw_arguments;
    entry.mode_ = mode;
    entry.collation_ = collation;
    entry.parser_name_len_ = parser_name.length();
    entry.properties_len_ = properties.length();
    entry.text_len_ = text.length();
    entry.result_len_ = result.length();
    MEMCPY(entry.parser_name_, parser_name.ptr(), parser_name.length());
    MEMCPY(entry.properties_, properties.ptr(), properties.length());
    MEMCPY(entry.text_, text.ptr(), text.length());
    MEMCPY(entry.result_, result.ptr(), result.length());
    entry.valid_ = true;
  }
}
} // namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (expr.arg_cnt_ <= 2) {
    // Without parser properties the only inputs are the text and the parser name, and both are
    // already decided by the arguments themselves. Read them straight out of the argument datums
    // so a repeated call is recognised before any parser object is built, and the whole parse
    // phase - parser construction, dictionary check, property reformatting - is skipped.
    ObDatum *text_datum = nullptr;
    ObDatum *name_datum = nullptr;
    ObString text;
    ObString name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    if (OB_FAIL(expr.args_[0]->eval(ctx, text_datum))) {
    } else if (1 < expr.arg_cnt_ && OB_FAIL(expr.args_[1]->eval(ctx, name_datum))) {
    } else {
      if (!text_datum->is_null()) {
        text = text_datum->get_string();
      }
      if (1 < expr.arg_cnt_ && !name_datum->is_null()) {
        name = name_datum->get_string().trim();
      }
      const ObString no_properties;
      const int64_t mode = static_cast<int64_t>(TokenizeParam::DEFAULT);
      const int64_t collation =
          static_cast<int64_t>(expr.args_[0]->obj_meta_.get_collation_type());
      const bool memo_eligible = tokenize_memo_fits(name, no_properties, text);
      ObString cached_result;
      if (memo_eligible
          && tokenize_memo_lookup(name, no_properties, text, mode, collation, true/*raw arguments*/, cached_result)) {
        if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, cached_result))) {
        }
      } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
      } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
      } else {
        ObString raw_result;
        const bool has_raw_result =
            (memo_eligible
             && OB_SUCCESS == share::ObJsonWrapper::get_raw_binary(json_result,
                                                                   raw_result,
                                                                   &temp_allocator)
             && 0 < raw_result.length()
             && raw_result.length() <= ObTokenizeMemoEntry::MAX_RESULT_LEN);
        if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                    ctx,
                                                    temp_allocator,
                                                    json_result,
                                                    expr_datum))) {
        } else if (has_raw_result) {
          tokenize_memo_store(name, no_properties, text, mode, collation, true/*raw arguments*/, raw_result);
        }
      }
    }
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else {
    ObString cached_result;
    // Arguments this large can never be held by an entry, so they take the plain path untouched.
    const bool memo_eligible = tokenize_memo_fits(param.parser_name_,
                                                  param.properties_,
                                                  param.fulltext_);
    if (memo_eligible
        && tokenize_memo_lookup(param.parser_name_,
                                param.properties_,
                                param.fulltext_,
                                static_cast<int64_t>(param.output_mode_),
                                static_cast<int64_t>(param.meta_.get_collation_type()),
                                false/*normalized arguments*/,
                                cached_result)) {
      // Same text, parser and options as a recent call on this thread: reuse its result.
      if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr,
                                                      ctx,
                                                      expr_datum,
                                                      cached_result))) {
      }
    } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
    } else {
      ObString raw_result;
      const bool has_raw_result =
          (memo_eligible
           && OB_SUCCESS == share::ObJsonWrapper::get_raw_binary(json_result,
                                                                 raw_result,
                                                                 &temp_allocator)
           && 0 < raw_result.length()
           && raw_result.length() <= ObTokenizeMemoEntry::MAX_RESULT_LEN);
      if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                  ctx,
                                                  temp_allocator,
                                                  json_result,
                                                  expr_datum))) {
      } else if (has_raw_result) {
        tokenize_memo_store(param.parser_name_,
                            param.properties_,
                            param.fulltext_,
                            static_cast<int64_t>(param.output_mode_),
                            static_cast<int64_t>(param.meta_.get_collation_type()),
                            false/*normalized arguments*/,
                            raw_result);
      }
    }
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  ObArenaAllocator tmp_parse_alloc(ObMemAttr("Tmp buffer"));

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_name_, param.properties_))) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (!param.properties_.empty()
             && OB_FAIL(param.reform_parser_properties(param.properties_))) {
    // Only the properties given by the user need to be validated and completed
    // with defaults. An empty property string already means "parser defaults"
    // for the parse helper, so skip building and re-parsing that default json
    // on every call.
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else if (is_null) {
      // A null property list carries no properties at all, exactly like leaving it out, and there
      // is no document to walk below.
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;

  if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  // The general dictionary loader loads the built in dictionaries once for the
  // whole process and never clears that state, and every call has to take its
  // global write lock to find that out. Remember the answer instead: this runs
  // on every tokenize() call, and after the first one there is nothing to do.
  static std::atomic<bool> dictionary_loaded(false);
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  // The loader also validates the input charset. Only its supported charset
  // may use the readiness cache; unsupported inputs must keep the loader error.
  const bool has_supported_charset =
      CHARSET_UTF8MB4 == ObCharset::charset_type_by_coll(meta_.get_collation_type());
  if (has_supported_charset && dictionary_loaded.load(std::memory_order_relaxed)) {
  } else if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    } else {
      // only remember the success, a transient failure has to be retried
      dictionary_loaded.store(true, std::memory_order_relaxed);
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
