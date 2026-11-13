// Empty properties must restore defaults when a helper is reused after custom settings.
#include <cstdio>
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "storage/fts/utils/unicode_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

int main()
{
  if (ObCharset::init_charset() != OB_SUCCESS) return 1;
  const char *names[] = {"ik", "space", "beng", "ngram", "ngram2"};
  const char *configs[] = {"{\"ik_mode\":\"max_word\"}",
      "{\"min_token_size\":5,\"max_token_size\":20}",
      "{\"min_token_size\":5,\"max_token_size\":20}",
      "{\"ngram_token_size\":3}",
      "{\"min_ngram_size\":3,\"max_ngram_size\":5}"};
  for (int i = 0; i < 5; ++i) {
    ObFTParser parser;
    parser.set_name_and_version(ObFTParserName(names[i]), ObFTParser::BUILTIN_VERSION);
    ObFTParserProperty reused, fresh;
    if (fresh.parse_for_parser_helper(parser, ObString()) != OB_SUCCESS
        || reused.parse_for_parser_helper(parser, ObString::make_string(configs[i])) != OB_SUCCESS
        || reused.is_equal(fresh)
        || reused.parse_for_parser_helper(parser, ObString()) != OB_SUCCESS
        || !reused.is_equal(fresh)) {
      std::fprintf(stderr, "default restore failed: %s\n", names[i]);
      return 2;
    }
  }
  // Independent pre-optimization numeral set. Exhaustively cover every Unicode
  // code point, including non-numerals sharing the fast-filter blocks.
  const ob_wc_t numerals[] = {0x4E00,0x4E8C,0x4E24,0x4E09,0x56DB,0x4E94,
      0x516D,0x4E03,0x516B,0x4E5D,0x5341,0x96F6,0x58F9,0x8D30,0x53C1,
      0x8086,0x4F0D,0x9646,0x67D2,0x634C,0x7396,0x62FE,0x767E,0x5343,
      0x4E07,0x4EBF,0x4F70,0x4EDF,0x842C,0x5104,0x5146,0x5345,0x5EFF};
  for (ob_wc_t point = 0; point <= 0x10FFFF; ++point) {
    bool expected = false;
    for (auto value : numerals) expected = expected || point == value;
    if (ObUnicodeBlockUtils::is_unicode_cn_number(point) != expected) return 3;
  }
  std::puts("Parser default reuse and exhaustive Unicode numeral classification: PASS");
  return 0;
}
