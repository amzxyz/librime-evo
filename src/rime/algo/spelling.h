//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2012-01-17 GONG Chen <chen.sst@gmail.com>
//

#ifndef RIME_SPELLING_H_
#define RIME_SPELLING_H_

#include <cstdint>
#include <rime/common.h>

namespace rime {

enum SpellingType {
  kNormalSpelling,
  kWeakSpelling,
  kAbbreviation,
  kCompletion,
  kAmbiguousSpelling,
  kInvalidSpelling
};

// Conditional fuzzy groups are independent of spelling quality.
constexpr size_t kMaxFuzzyGroups = 16;
constexpr uint32_t kFuzzyGroupMask = (1u << kMaxFuzzyGroups) - 1;

struct SpellingProperties {
  SpellingType type = kNormalSpelling;
  size_t end_pos = 0;
  double credibility = 0.0;
  string tips;
  bool is_correction = false;
  uint32_t fuzzy_mask = 0;
  bool auxiliary = false;  // AUX hit; may carry a rule-specific penalty
  bool auxiliary_display = false;

  // 疊加運算規則, 將運算規則產生的屬性應用到當前拼寫上
  void Compose(const SpellingProperties& delta);
  // 更新優化拼寫, 將另一個候選拼寫合併到當前記錄
  void Update(const SpellingProperties& other);
};

struct Spelling {
  string str;
  SpellingProperties properties;

  Spelling() = default;
  Spelling(const string& _str) : str(_str) {}

  bool operator==(const Spelling& other) { return str == other.str; }
  bool operator<(const Spelling& other) { return str < other.str; }
};

}  // namespace rime

#endif  // RIME_SPELLING_H_
