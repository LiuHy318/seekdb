// Exercise the real IK prefix walker with an allocator that poisons retired
// list nodes. Link against the configured seekdb libraries; this requires no
// server, dictionary files or SQL fixture.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef IK_PREFIX_TEST_LEGACY
#define ObIKCJKProcessor LegacyIKCJKProcessor
#endif
#include "storage/fts/ik/ob_ik_cjk_processor.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

class PoisonAllocator final : public ObIAllocator
{
public:
  ~PoisonAllocator() override
  {
    for (const auto &block : blocks_) {
      std::free(block.ptr_);
    }
  }
  void *alloc(const int64_t size) override
  {
    void *ptr = std::malloc(size);
    if (ptr != nullptr) {
      blocks_.push_back({ptr, size, false});
    }
    return ptr;
  }
  void *alloc(const int64_t size, const oceanbase::lib::ObMemAttr &) override
  {
    return alloc(size);
  }
  void free(void *ptr) override
  {
    for (auto &block : blocks_) {
      if (block.ptr_ == ptr && !block.retired_) {
        // Retain the allocation until teardown, but invalidate every field.
        // Advancing an erased iterator must never read its old next pointer.
        std::memset(ptr, 0, block.size_);
        block.retired_ = true;
        ++retired_;
        return;
      }
    }
    std::abort();
  }
  int64_t retired_ = 0;
private:
  struct Block { void *ptr_; int64_t size_; bool retired_; };
  std::vector<Block> blocks_;
};

class ExpiringPrefixDict final : public ObIFTDict
{
public:
  int init() override { return OB_SUCCESS; }
  int match(const ObString &, bool &is_match) const override
  {
    is_match = false;
    return OB_SUCCESS;
  }
  int match(const ObString &, ObDATrieHit &hit) const override
  {
    hit.set_prefix();
    return OB_SUCCESS;
  }
  int match_with_hit(const ObString &, const ObDATrieHit &, ObDATrieHit &hit) const override
  {
    ++expired_;
    hit.set_unmatch();
    return OB_SUCCESS;
  }
  mutable int64_t expired_ = 0;
};

int main()
{
  if (ObCharset::init_charset() != OB_SUCCESS) {
    return 1;
  }
  PoisonAllocator allocator;
  ExpiringPrefixDict dict;
  const char text[] = "甲乙丙丁戊己庚辛";
  TokenizeContext ctx(CS_TYPE_UTF8MB4_GENERAL_CI, allocator, text, sizeof(text) - 1, false);
  ObIKCJKProcessor processor(dict, allocator);
  if (ctx.init() != OB_SUCCESS) {
    return 2;
  }
  int64_t seen = 0;
  while (!ctx.iter_end()) {
    const char *ch = nullptr;
    uint8_t len = 0;
    if (ctx.current_char(ch, len) != OB_SUCCESS
        || processor.do_process(ctx, ch, len, ObFTCharUtil::CharType::CHINESE) != OB_SUCCESS) {
      return 3;
    }
    ++seen;
    const int step_ret = ctx.step_next();
    if (step_ret != OB_SUCCESS && !(step_ret == OB_ITER_END && ctx.iter_end())) {
      return 3;
    }
  }
  if (seen != 8 || dict.expired_ != 7 || allocator.retired_ < 7) {
    return 4;
  }
  std::puts("IK prefix lifetime: PASS (7 retired prefixes, poisoned nodes)");
  return 0;
}
