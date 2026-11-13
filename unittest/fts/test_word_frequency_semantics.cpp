// Exercise ObAddWord's public frequency semantics without a running server.
#include <cstdio>
#include <string>
#include <vector>
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_property.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

int check_grouping(ObCollationType collation, bool case_insensitive)
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  if (words.create(2, ObMemAttr("FTWordTest")) != OB_SUCCESS) return 1;
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(collation);
  ObFTParserProperty property;
  ObAddWordFlag flag;
  flag.set_groupby_word();
  ObAddWord add(property, meta, flag, allocator, words);
  const char first[] = "Alpha";
  const char repeated[] = "alpha";
  if (add.process_word(first, 5, 5, 7) != OB_SUCCESS
      || add.process_word(repeated, 5, 5, 3) != OB_SUCCESS) return 2;
  int64_t *count = words.get(ObFTWord(5, first, meta));
  if (count == nullptr || *count != (case_insensitive ? 4 : 1)
      || words.size() != (case_insensitive ? 1 : 2)
      || add.get_add_word_count() != 10) return 3;
  if (add.process_word(first, 5, 5, 0) != OB_INVALID_ARGUMENT
      || add.process_word(first, 5, 5, -1) != OB_INVALID_ARGUMENT
      || add.process_word(first, 0, 0, 1) != OB_INVALID_ARGUMENT
      || add.get_add_word_count() != 10) return 4;

  // Keep the strings alive and their addresses stable while the map borrows them.
  std::vector<std::string> keys;
  keys.reserve(2000);
  for (int i = 0; i < 2000; ++i) {
    keys.push_back("term_" + std::to_string(i));
    const auto &key = keys.back();
    if (add.process_word(key.data(), key.size(), key.size(), 5) != OB_SUCCESS
        || add.process_word(key.data(), key.size(), key.size(), 2) != OB_SUCCESS) return 5;
  }
  if (words.size() != 2000 + (case_insensitive ? 1 : 2)
      || add.get_add_word_count() != 14010) return 6;
  for (const auto &key : keys) {
    count = words.get(ObFTWord(key.size(), key.data(), meta));
    if (count == nullptr || *count != 3) return 7;
  }
  if (case_insensitive) {
    bool retained_original = false;
    for (auto it = words.begin(); it != words.end(); ++it) {
      if (it->first == ObFTWord(5, first, meta)) {
        retained_original = it->first.get_word().get_string() == ObString(5, first);
      }
    }
    if (!retained_original) return 8;
  }
  return 0;
}

int check_ungrouped()
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  if (words.create(2, ObMemAttr("FTWordTest")) != OB_SUCCESS) return 10;
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObFTParserProperty property;
  ObAddWordFlag flag;
  ObAddWord add(property, meta, flag, allocator, words);
  const char text[] = "duplicate";
  if (add.process_word(text, 9, 9, 7) != OB_SUCCESS
      || add.process_word(text, 9, 9, 3) != OB_HASH_EXIST
      || add.get_add_word_count() != 7) return 11;
  int64_t *count = words.get(ObFTWord(9, text, meta));
  if (words.size() != 1 || count == nullptr || *count != 1) return 12;
  return 0;
}

int main()
{
  if (ObCharset::init_charset() != OB_SUCCESS) return 20;
  for (auto collation : {CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN}) {
    const int ret = check_grouping(collation, collation == CS_TYPE_UTF8MB4_GENERAL_CI);
    if (ret != 0) return ret;
  }
  const int ret = check_ungrouped();
  if (ret != 0) return ret;
  std::puts("Word frequency semantics: PASS (first/repeated frequency, collation, collisions, invalid input)");
  return 0;
}
