// Exercise entry rejection, byte-triggered shard eviction, and concurrent replay.
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "storage/fts/ob_ft_token_cache.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

int main()
{
  if (ObCharset::init_charset() != OB_SUCCESS) return 1;
  ObFTTokenCache cache;
  if (cache.init() != OB_SUCCESS) return 2;
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObFTParserProperty property;
  ObFTParserName parser("space");
  ObFTWordMap words;
  if (words.create(2, ObMemAttr("FTCacheTest")) != OB_SUCCESS
      || words.set_refactored(ObFTWord(5, "token", meta), 3) != OB_SUCCESS) return 3;
  auto key_for = [](uint64_t hash, int64_t len) {
    ObFTTokenCacheKey key;
    key.text_hash_ = hash;
    key.text_len_ = len;
    key.collation_ = CS_TYPE_UTF8MB4_BIN;
    return key;
  };
  auto lookup = [&](const ObFTTokenCacheKey &key, const std::string &text, bool &hit) {
    ObArenaAllocator allocator;
    ObFTWordMap replay;
    int64_t doc_len = 0;
    if (replay.create(2, ObMemAttr("FTCacheTest")) != OB_SUCCESS) return false;
    const int ret = cache.try_get(key, parser, 1, property, text.data(), text.size(),
                                  allocator, replay, doc_len, hit);
    const int64_t *count = replay.get(ObFTWord(5, "token", meta));
    return ret == OB_SUCCESS && (!hit || (doc_len == 3 && replay.size() == 1
                                         && count != nullptr && *count == 3));
  };
  std::string large(128 * 1024, 'x');
  auto large_key = key_for(1, large.size());
  bool hit = false;
  if (cache.try_store(large_key, parser, 1, property, large.data(), large.size(), 3, words)
          != OB_SUCCESS || !lookup(large_key, large, hit) || hit) return 4;

  // Force one shard over its byte limit without approaching the old global
  // 131072-entry limit. The oldest payload must be gone, the newest readable.
  ObFTTokenCacheKey first, last;
  std::string text(4096, 'a');
  int inserted = 0;
  for (uint64_t n = 0; inserted < 160; ++n) {
    auto key = key_for(n, text.size());
    uint64_t hash = 0;
    key.hash(hash);
    if ((hash & 255) != 0) continue;
    if (inserted == 0) first = key;
    if (cache.try_store(key, parser, 1, property, text.data(), text.size(), 3, words)
            != OB_SUCCESS) return 5;
    last = key;
    ++inserted;
  }
  if (!lookup(first, text, hit) || hit || !lookup(last, text, hit) || !hit) return 6;

  std::atomic<int> failures(0);
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      // All workers contend on the same shard and repeatedly trigger eviction.
      for (int i = 0; i < 320; ++i) {
        const uint64_t n = static_cast<uint64_t>(worker * 1000000 + i);
        auto key = key_for(n, text.size());
        uint64_t hash = 0;
        do { key.text_hash_++; key.hash(hash); } while ((hash & 255) != 0);
        bool found = false;
        if (cache.try_store(key, parser, 1, property, text.data(), text.size(), 3, words)
                != OB_SUCCESS || !lookup(key, text, found)) ++failures;
      }
    });
  }
  for (auto &worker : workers) worker.join();
  cache.destroy();
  if (failures.load() != 0) return 7;
  std::puts("Token cache bounds and concurrent shard eviction: PASS");
  return 0;
}
