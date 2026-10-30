//
// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_AUXILIARY_MATCHER_H_
#define RIME_AUXILIARY_MATCHER_H_

#include <rime/common.h>
#include <rime/algo/syllabifier.h>

namespace rime {

class Dictionary;
class ReverseLookupDictionary;

// Shared AUX lookup and matching logic for direct and indirect AUX input.
class AuxiliaryMatcher {
 public:
  AuxiliaryMatcher(Dictionary* primary,
                   ReverseLookupDictionary* reverse = nullptr);

  void Lookup(const string& input, vector<AuxGraphMatch>* result);
  bool LookupAuxiliary(const string& text, vector<string>* result);
  bool Match(const string& text, const string& input);

 private:
  bool MatchSequence(const vector<vector<string>>& codes,
                     size_t index,
                     const string& input,
                     size_t input_index,
                     map<pair<size_t, size_t>, bool>* memo) const;

  Dictionary* primary_ = nullptr;
  ReverseLookupDictionary* reverse_ = nullptr;
  map<string, SyllableId> main_ids_;
  map<string, std::shared_ptr<const std::set<string>>> permitted_chars_;
  map<string, vector<string>> auxiliary_cache_;
};

}  // namespace rime

#endif  // RIME_AUXILIARY_MATCHER_H_
