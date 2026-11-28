//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-05-16 Zou Xu <zouivex@gmail.com>
// 2012-01-26 GONG Chen <chen.sst@gmail.com>  spelling algebra support
//

#ifndef RIME_PRISM_H_
#define RIME_PRISM_H_

#include <string_view>
#include <darts.h>
#include <rime/common.h>
#include <rime/algo/spelling.h>
#include <rime/dict/mapped_file.h>
#include <rime/dict/vocabulary.h>

namespace rime {

namespace prism {

using Credibility = float;

struct SpellingDescriptor {
  SyllableId syllable_id;
  // bits 0-3: type; 4-19: fuzzy requirements; 30: correction.
  uint32_t flags;
  Credibility credibility;
  String tips;
};

using SpellingMapItem = List<SpellingDescriptor>;
using SpellingMap = Array<SpellingMapItem>;

struct Metadata {
  static const int kFormatMaxLength = 32;
  char format[kFormatMaxLength];
  uint32_t dict_file_checksum;
  uint32_t schema_file_checksum;
  uint32_t num_syllables;
  uint32_t num_spellings;
  uint32_t double_array_size;
  OffsetPtr<char> double_array;
  // v1.0
  OffsetPtr<SpellingMap> spelling_map;
  char alphabet[256];
  // v5.0
  uint32_t max_key_length;
  // v6.0: shared code namespace and automatic supplement association.
  List<String> syllabary;
  uint32_t has_packs;
  // v7.0: dense spelling offsets followed by shared choice records.
  List<uint32_t> code_choices;
  List<char> code_choice_strings;
  // v8.0: stable IDs for conditional spelling groups.
  List<String> fuzzy_groups;
};

// Borrowed view of a compiled choice list, valid while its Prism is mapped.
class CodeChoiceView {
 public:
  CodeChoiceView() = default;
  CodeChoiceView(const uint32_t* offsets, const char* strings, size_t size)
      : offsets_(offsets), strings_(strings), size_(size) {}
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  std::string_view operator[](size_t i) const { return strings_ + offsets_[i]; }

 private:
  const uint32_t* offsets_ = nullptr;
  const char* strings_ = nullptr;
  size_t size_ = 0;
};

}  // namespace prism

class SpellingAccessor {
 public:
  SpellingAccessor(prism::SpellingMap* spelling_map,
                   SyllableId spelling_id,
                   uint32_t enabled_fuzzy = 0,
                   bool conditional = true);
  RIME_DLL bool Next();
  RIME_DLL bool exhausted() const;
  RIME_DLL SyllableId syllable_id() const;
  RIME_DLL SpellingProperties properties() const;

 protected:
  SyllableId spelling_id_;
  prism::SpellingDescriptor* iter_;
  prism::SpellingDescriptor* end_;
  prism::SpellingDescriptor* next_ = nullptr;
  uint32_t enabled_fuzzy_ = 0;
  bool conditional_ = true;
  void SelectNext();
};

class Script;

class Prism : public MappedFile {
 public:
  using Match = Darts::DoubleArray::result_pair_type;

  RIME_DLL explicit Prism(const path& file_path);

  RIME_DLL bool Load();
  RIME_DLL bool Save();
  RIME_DLL bool Build(const Syllabary& syllabary,
                      const Script* script = nullptr,
                      uint32_t dict_file_checksum = 0,
                      uint32_t schema_file_checksum = 0,
                      bool has_packs = false,
                      const vector<string>& fuzzy_groups = {});

  RIME_DLL bool HasKey(std::string_view key);
  RIME_DLL bool GetValue(std::string_view key, int* value) const;
  RIME_DLL void CommonPrefixSearch(std::string_view key, vector<Match>* result);
  RIME_DLL void ExpandSearch(std::string_view key,
                             vector<Match>* result,
                             size_t limit,
                             uint32_t enabled_fuzzy = 0,
                             SpellingType max_type = kInvalidSpelling);
  // Strict refinements of compiled spelling sets; no keyboard configuration.
  RIME_DLL bool HasCodeChoices() const;
  RIME_DLL prism::CodeChoiceView QueryCodeChoices(int spelling_id) const;
  RIME_DLL prism::CodeChoiceView QueryCodeChoices(std::string_view key) const;
  RIME_DLL uint32_t FuzzyMask(const vector<string>& groups) const;
  bool HasConditionalSpellings() const {
    return IsOpen() && format_ >= 8.0 && metadata_ &&
           metadata_->fuzzy_groups.size > 0;
  }
  RIME_DLL SpellingAccessor QuerySpelling(SyllableId spelling_id,
                                          uint32_t enabled_fuzzy = 0);

  bool has_syllabary() const { return format_ >= 6.0; }
  bool has_code_choice_format() const { return format_ >= 7.0; }
  bool has_packs() const {
    return has_syllabary() && metadata_ && metadata_->has_packs;
  }
  RIME_DLL bool GetSyllabary(Syllabary* result) const;
  RIME_DLL string GetSyllableById(SyllableId id) const;
  RIME_DLL size_t array_size() const;
  RIME_DLL size_t max_key_length() const;
  uint32_t dict_file_checksum() const;
  uint32_t schema_file_checksum() const;
  Darts::DoubleArray& trie() const { return *trie_; }

 protected:
  the<Darts::DoubleArray> trie_;
  prism::Metadata* metadata_ = nullptr;
  prism::SpellingMap* spelling_map_ = nullptr;
  double format_ = 0.0;
};

}  // namespace rime

#endif  // RIME_PRISM_H_
