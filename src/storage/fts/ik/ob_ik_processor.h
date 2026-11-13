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

#ifndef _OCEANBASE_STORAGE_FTS_IK_OB_IK_PROCESSOR_H_
#define _OCEANBASE_STORAGE_FTS_IK_OB_IK_PROCESSOR_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/charset/ob_charset.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
class TokenizeContext
{
public:
  TokenizeContext(ObCollationType coll_type,
                  ObIAllocator &allocator,
                  const char *fulltext,
                  int64_t fulltext_len,
                  bool is_smart);

  ~TokenizeContext();

  int init();
  int reset_resource();

  int get_next_token(const char *&word, int64_t &word_len, int64_t &offset, int64_t &char_cnt);

  int compound(ObIKToken &result);

  int current_char(const char *&ch, uint8_t &char_len);
  int current_char_type(ObFTCharUtil::CharType &type);

  // Decode and classify the character starting at pos in one pass. The
  // arbitrator walks the document again after segmentation to fill the gaps
  // between tokens, and first_valid_char plus classify_first_char decode the
  // same character twice.
  int char_at(int64_t pos, int64_t &char_len, ObFTCharUtil::CharType &type) const;

  int check_current_cn_number(bool &is_number) const
  {
    if (has_decoded_unicode_) {
      is_number = ObUnicodeBlockUtils::is_unicode_cn_number(next_unicode_);
      return OB_SUCCESS;
    }
    return ObFTCharUtil::check_cn_number(coll_type_, fulltext_ + cursor_, next_char_len_, is_number);
  }
  int check_current_num_connector(bool &is_connector) const
  {
    if (has_decoded_unicode_) {
      is_connector = ObUnicodeBlockUtils::check_num_connector(next_unicode_);
      return OB_SUCCESS;
    }
    return ObFTCharUtil::check_num_connector(coll_type_, fulltext_ + cursor_, next_char_len_, is_connector);
  }
  int check_current_letter_connector(bool &is_connector) const
  {
    if (has_decoded_unicode_) {
      is_connector = ObUnicodeBlockUtils::check_letter_connector(next_unicode_);
      return OB_SUCCESS;
    }
    return ObFTCharUtil::check_letter_connector(coll_type_, fulltext_ + cursor_, next_char_len_, is_connector);
  }

  int step_next();

  ObCollationType collation() const;
  // The collation resolved once for the whole walk of the document. A tokenizer
  // asks for the next character, and for its class, on every character of a
  // document; resolving the collation at each of those is pure repetition.
  const ObCharsetInfo *charset_info() const { return cs_; }
  int64_t get_end_cursor() const;
  const char *fulltext() const;
  int64_t fulltext_len() const;
  int64_t get_cursor() const;

  bool is_last() const;
  bool iter_end() const;
  bool is_smart() const;

  int add_chain(ObIKTokenChain *chain);
  int add_token(const char *fulltext,
                int64_t offset,
                int64_t length,
                int64_t char_cnt,
                ObIKTokenType type);

  ObFTSortList &token_list() { return token_list_; }

  ObList<ObIKToken, ObIAllocator> &result_list() { return result_list_; }

  int32_t handle_size() const { return handle_size_; }

private:
  int prepare_next_char();

  ObCollationType coll_type_;
  const ObCharsetInfo *cs_;
  const char *fulltext_;
  int64_t fulltext_len_;

  int64_t cursor_;
  int64_t next_char_len_;
  ObFTCharUtil::CharType next_char_type_;
  ob_wc_t next_unicode_;
  const bool is_utf8mb4_;
  bool has_decoded_unicode_;

  uint32_t handle_size_;
  bool is_smart_;

  ObFTSortList token_list_;
  ObList<ObIKToken, ObIAllocator> result_list_;

private:
  DISALLOW_COPY_AND_ASSIGN(TokenizeContext);
};

class ObIIKProcessor
{
public:
  ObIIKProcessor() {}

  virtual ~ObIIKProcessor() {}

  int process(TokenizeContext &ctx);

  virtual int do_process(TokenizeContext &ctx,
                         const char *ch,
                         const uint8_t char_len,
                         const ObFTCharUtil::CharType type)
      = 0;
};

} // namespace storage
} // namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_IK_OB_IK_PROCESSOR_H_
