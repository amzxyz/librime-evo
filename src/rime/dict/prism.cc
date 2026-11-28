//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-05-16 Zou Xu <zouivex@gmail.com>
// 2012-01-26 GONG Chen <chen.sst@gmail.com>  spelling algebra support
//
#include <algorithm>
#include <cfloat>
#include <cstring>
#include <limits>
#include <queue>
#include <rime/algo/algebra.h>
#include <rime/dict/prism.h>

namespace rime {

namespace {

struct node_t {
  node_t(size_t node_pos, size_t key_length)
      : node_pos(node_pos), key_length(key_length) {}

  size_t node_pos;
  size_t key_length;
};

// SpellingDescriptor::flags stores quality, fuzzy requirements and correction.
constexpr uint32_t kTypeIsCorrectionMask = 1u << 30;
constexpr uint32_t kSpellingTypeMask = 0xf;
constexpr unsigned kFuzzyShift = 4;
uint32_t RequiredFuzzy(uint32_t flags) {
  return (flags >> kFuzzyShift) & kFuzzyGroupMask;
}
bool Enabled(uint32_t flags, uint32_t mask) {
  const auto required = RequiredFuzzy(flags);
  return (required & mask) == required;
}
bool BetterDescriptor(const prism::SpellingDescriptor& a,
                      const prism::SpellingDescriptor& b) {
  const auto at = a.flags & kSpellingTypeMask;
  const auto bt = b.flags & kSpellingTypeMask;
  if (at != bt)
    return at < bt;
  const bool ac = (a.flags & kTypeIsCorrectionMask) != 0;
  const bool bc = (b.flags & kTypeIsCorrectionMask) != 0;
  if (ac != bc)
    return !ac;
  return a.credibility > b.credibility;
}

struct CodeChoiceData {
  vector<uint32_t> words;
  vector<char> strings;
};

// Compute the strict subset relation while the complete Script is available.
// The trie is built from the same ordered keys, so its values are the indices
// in this table. No reverse trie traversal or runtime index is necessary.
bool BuildCodeChoiceData(const Script* script,
                         const map<string, SyllableId>& syllable_to_id,
                         const vector<const char*>& keys,
                         CodeChoiceData* data) {
  if (!script || std::none_of(script->begin(), script->end(),
                              [](const Script::value_type& entry) {
                                return entry.second.size() > 1;
                              }))
    return true;

  struct Group {
    vector<SyllableId> ids;
    vector<size_t> refinements;
    size_t representative;
  };
  const size_t invalid = static_cast<size_t>(-1);
  const size_t max_offset = (std::numeric_limits<uint32_t>::max)();
  vector<size_t> group_by_spelling(keys.size(), invalid);
  vector<Group> groups;
  hash_map<vector<SyllableId>, size_t> group_ids;
  size_t spelling_id = 0;
  for (const auto& entry : *script) {
    const size_t index = spelling_id++;
    vector<SyllableId> ids;
    ids.reserve(entry.second.size());
    for (const auto& spelling : entry.second) {
      if (spelling.properties.fuzzy_mask)
        continue;
      if (spelling.properties.is_correction ||
          spelling.properties.type >= kAbbreviation) {
        ids.clear();
        break;
      }
      auto found = syllable_to_id.find(spelling.str);
      if (found == syllable_to_id.end())
        return false;
      ids.push_back(found->second);
    }
    if (ids.empty())
      continue;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    auto inserted = group_ids.emplace(ids, groups.size());
    if (inserted.second)
      groups.push_back({std::move(ids), {}, index});
    group_by_spelling[index] = inserted.first->second;
  }

  // Dense syllable IDs make a vector of posting lists cheaper than a hash.
  // Start with the rarest target ID, then verify exact integer containment.
  vector<vector<size_t>> postings(syllable_to_id.size());
  for (size_t i = 0; i < groups.size(); ++i) {
    if (groups[i].ids.size() > 1) {
      for (auto id : groups[i].ids)
        postings[id].push_back(i);
    }
  }
  bool has_refinements = false;
  for (size_t i = 0; i < groups.size(); ++i) {
    const auto& target = groups[i].ids;
    const vector<size_t>* candidates = &postings[target.front()];
    for (auto id : target) {
      if (postings[id].size() < candidates->size())
        candidates = &postings[id];
      if (candidates->empty())
        break;
    }
    for (size_t source : *candidates) {
      const auto& ids = groups[source].ids;
      if (ids.size() > target.size() &&
          std::includes(ids.begin(), ids.end(), target.begin(), target.end())) {
        groups[source].refinements.push_back(i);
        has_refinements = true;
      }
    }
  }
  if (!has_refinements)
    return true;
  if (keys.size() >= max_offset)
    return false;

  // First N words are directly indexed by spelling ID. Zero means no list.
  // Each list contains its length followed by offsets into the string pool.
  // Equal source sets and repeated target strings share storage.
  data->words.resize(keys.size());
  vector<uint32_t> list_offsets(groups.size());
  vector<size_t> string_offsets(groups.size(), invalid);
  for (size_t i = 0; i < groups.size(); ++i) {
    const auto& targets = groups[i].refinements;
    if (targets.empty())
      continue;
    if (targets.size() >= max_offset - data->words.size())
      return false;
    list_offsets[i] = static_cast<uint32_t>(data->words.size());
    data->words.push_back(static_cast<uint32_t>(targets.size()));
    // Groups are created in Script key order, so targets are already sorted.
    for (size_t target : targets) {
      auto& offset = string_offsets[target];
      if (offset == invalid) {
        const char* key = keys[groups[target].representative];
        const size_t bytes = std::strlen(key) + 1;
        if (bytes > max_offset - data->strings.size())
          return false;
        offset = data->strings.size();
        data->strings.insert(data->strings.end(), key, key + bytes);
      }
      data->words.push_back(static_cast<uint32_t>(offset));
    }
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    const size_t group = group_by_spelling[i];
    if (group != invalid)
      data->words[i] = list_offsets[group];
  }
  return true;
}

}  // namespace

const char kPrismFormat[] = "Rime::Prism/8.0";
const double kPrismFormatVersion = 8.0;

const char kPrismFormatPrefix[] = "Rime::Prism/";
const size_t kPrismFormatPrefixLen = sizeof(kPrismFormatPrefix) - 1;

const char kDefaultAlphabet[] = "abcdefghijklmnopqrstuvwxyz";

SpellingAccessor::SpellingAccessor(prism::SpellingMap* spelling_map,
                                   SyllableId spelling_id,
                                   uint32_t enabled_fuzzy,
                                   bool conditional)
    : spelling_id_(spelling_id),
      iter_(nullptr),
      end_(nullptr),
      enabled_fuzzy_(enabled_fuzzy),
      conditional_(conditional) {
  if (spelling_id < 0) {
    spelling_id_ = -1;
    return;
  }
  if (spelling_map) {
    if (spelling_id >= static_cast<SyllableId>(spelling_map->size)) {
      spelling_id_ = -1;
      return;
    }
    iter_ = spelling_map->at[spelling_id].begin();
    end_ = spelling_map->at[spelling_id].end();
    if (conditional_)
      SelectNext();
    else if (iter_ == end_)
      spelling_id_ = -1;
  }
}

void SpellingAccessor::SelectNext() {
  while (iter_ && iter_ < end_) {
    auto* current = iter_;
    prism::SpellingDescriptor* best = nullptr;
    // Build stores all alternative paths for one syllable consecutively.
    do {
      if (Enabled(current->flags, enabled_fuzzy_) &&
          (!best || BetterDescriptor(*current, *best)))
        best = current;
      ++current;
    } while (current < end_ && current->syllable_id == iter_->syllable_id);
    next_ = current;
    if (best) {
      iter_ = best;
      return;
    }
    iter_ = next_;
  }
  spelling_id_ = -1;
}

bool SpellingAccessor::Next() {
  if (exhausted())
    return false;
  if (conditional_) {
    iter_ = next_;
    SelectNext();
  } else if (!iter_ || ++iter_ >= end_) {
    spelling_id_ = -1;
  }
  return exhausted();
}

bool SpellingAccessor::exhausted() const {
  return spelling_id_ == -1;
}

SyllableId SpellingAccessor::syllable_id() const {
  if (iter_ && iter_ < end_)
    return iter_->syllable_id;
  else
    return spelling_id_;
}

SpellingType SpellingAccessor::type() const {
  return iter_ && iter_ < end_
             ? static_cast<SpellingType>(iter_->flags & kSpellingTypeMask)
             : kNormalSpelling;
}

SpellingProperties SpellingAccessor::properties() const {
  SpellingProperties props;
  if (iter_ && iter_ < end_) {
    const uint32_t packed_type = iter_->flags;
    props.type = static_cast<SpellingType>(packed_type & kSpellingTypeMask);
    props.is_correction = (packed_type & kTypeIsCorrectionMask) != 0;
    props.fuzzy_mask = RequiredFuzzy(packed_type);
    props.credibility = iter_->credibility;
    if (!iter_->tips.empty()) {
      string hints = iter_->tips.c_str();
      props.tips = std::move(hints);
    }
  }
  return props;
}

Prism::Prism(const path& file_path)
    : MappedFile(file_path), trie_(new Darts::DoubleArray) {}

bool Prism::Load() {
  metadata_ = nullptr;
  spelling_map_ = nullptr;
  format_ = 0.0;
  LOG(INFO) << "loading prism file: " << file_path();

  if (IsOpen())
    Close();

  if (!OpenReadOnly()) {
    LOG(ERROR) << "error opening prism file '" << file_path() << "'.";
    return false;
  }

  metadata_ = Find<prism::Metadata>(0);
  if (!metadata_) {
    LOG(ERROR) << "metadata not found.";
    Close();
    return false;
  }
  if (strncmp(metadata_->format, kPrismFormatPrefix, kPrismFormatPrefixLen)) {
    LOG(ERROR) << "invalid metadata.";
    Close();
    return false;
  }
  format_ = atof(&metadata_->format[kPrismFormatPrefixLen]);

  // 版本檢查: 強制重構舊版本
  if (format_ < kPrismFormatVersion - DBL_EPSILON) {
    LOG(INFO) << "prism format " << format_ << " is too old. upgrading to "
              << kPrismFormatVersion;
    Close();
    return false;
  }

  char* array = metadata_->double_array.get();
  if (!array) {
    LOG(ERROR) << "double array image not found.";
    Close();
    return false;
  }
  size_t array_size = metadata_->double_array_size;
  LOG(INFO) << "found double array image of size " << array_size << ".";
  trie_->set_array(array, array_size);

  spelling_map_ = NULL;
  if (format_ > 1.0 - DBL_EPSILON) {
    spelling_map_ = metadata_->spelling_map.get();
  }
  return true;
}

bool Prism::Save() {
  LOG(INFO) << "saving prism file: " << file_path();
  if (!trie_->total_size()) {
    LOG(ERROR) << "the trie has not been constructed!";
    return false;
  }
  return ShrinkToFit();
}
bool Prism::Build(const Syllabary& syllabary,
                  const Script* script,
                  uint32_t dict_file_checksum,
                  uint32_t schema_file_checksum,
                  bool has_packs,
                  const vector<string>& fuzzy_groups) {
  metadata_ = nullptr;
  spelling_map_ = nullptr;
  format_ = 0.0;
  // building double-array trie
  size_t num_syllables = syllabary.size();
  size_t num_spellings = script ? script->size() : syllabary.size();
  vector<const char*> keys(num_spellings);
  size_t key_id = 0;
  size_t map_size = 0;
  size_t tips_bytes = 0;
  size_t max_key_length = 0;
  bool conditional = false;
  if (fuzzy_groups.size() > kMaxFuzzyGroups ||
      set<string>(fuzzy_groups.begin(), fuzzy_groups.end()).size() !=
          fuzzy_groups.size())
    return false;
  const uint32_t allowed = (1u << fuzzy_groups.size()) - 1;
  if (!num_spellings)
    return false;
  if (script) {
    for (auto it = script->begin(); it != script->end(); ++it, ++key_id) {
      keys[key_id] = it->first.c_str();
      max_key_length = (std::max)(max_key_length, it->first.length());
      map_size += it->second.size();
      for (const auto& spelling : it->second) {
        if (spelling.properties.fuzzy_mask & ~allowed)
          return false;
        conditional |= spelling.properties.fuzzy_mask != 0;
        if (!spelling.properties.tips.empty())
          tips_bytes += spelling.properties.tips.size() + 1;
      }
    }
  } else {
    for (auto it = syllabary.begin(); it != syllabary.end(); ++it, ++key_id) {
      keys[key_id] = it->c_str();
      max_key_length = (std::max)(max_key_length, it->length());
    }
  }
  map<string, SyllableId> syllable_to_id;
  if (script) {
    SyllableId id = 0;
    for (const auto& syllable : syllabary)
      syllable_to_id.emplace(syllable, id++);
  }
  CodeChoiceData choice_data;
  if (!BuildCodeChoiceData(script, syllable_to_id, keys, &choice_data)) {
    LOG(ERROR) << "error building prism code choice index.";
    return false;
  }
  if (0 != trie_->build(num_spellings, &keys[0])) {
    LOG(ERROR) << "Error building double-array trie.";
    return false;
  }
  // creating prism file
  size_t array_size = trie_->size();
  size_t image_size = trie_->total_size();
  // Reserve the actual tips and choice data before mapping the file.
  // Growing a mapped file would invalidate previously allocated pointers.
  size_t estimated_map_size =
      num_spellings * 12 + map_size * (4 + sizeof(prism::SpellingDescriptor)) +
      tips_bytes;
  const size_t choice_bytes =
      choice_data.words.size() * sizeof(uint32_t) + choice_data.strings.size();
  const size_t kReservedSize = 1024;
  size_t code_bytes = syllabary.size() * sizeof(String);
  for (const auto& code : syllabary)
    code_bytes += code.size() + 1;
  size_t fuzzy_bytes = fuzzy_groups.size() * sizeof(String);
  for (const auto& group : fuzzy_groups)
    fuzzy_bytes += group.size() + 1;
  if (!Create(image_size + estimated_map_size + code_bytes + choice_bytes +
              fuzzy_bytes + kReservedSize)) {
    LOG(ERROR) << "Error creating prism file '" << file_path() << "'.";
    return false;
  }
  // creating metadata
  auto metadata = Allocate<prism::Metadata>();
  if (!metadata) {
    LOG(ERROR) << "Error creating metadata in file '" << file_path() << "'.";
    return false;
  }
  metadata->dict_file_checksum = dict_file_checksum;
  metadata->schema_file_checksum = schema_file_checksum;
  metadata->num_syllables = num_syllables;
  metadata->num_spellings = num_spellings;
  metadata->max_key_length = max_key_length;
  metadata_ = metadata;
  metadata->has_packs = has_packs;
  auto codes = Allocate<String>(syllabary.size());
  if (!codes)
    return false;
  metadata->syllabary.at = codes;
  metadata->syllabary.size = syllabary.size();
  size_t code_index = 0;
  for (const auto& code : syllabary) {
    if (!CopyString(code, &codes[code_index++]))
      return false;
  }
  if (!fuzzy_groups.empty()) {
    auto groups = Allocate<String>(fuzzy_groups.size());
    if (!groups)
      return false;
    metadata->fuzzy_groups.at = groups;
    metadata->fuzzy_groups.size = fuzzy_groups.size();
    for (size_t i = 0; i < fuzzy_groups.size(); ++i) {
      if (!CopyString(fuzzy_groups[i], &groups[i]))
        return false;
    }
  }
  // alphabet
  {
    set<char> alphabet;
    for (size_t i = 0; i < num_spellings; ++i)
      for (const char* p = keys[i]; *p; ++p)
        alphabet.insert(*p);
    char* p = metadata->alphabet;
    set<char>::const_iterator c = alphabet.begin();
    for (; c != alphabet.end(); ++p, ++c)
      *p = *c;
    *p = '\0';
  }
  // saving double-array image
  char* array = Allocate<char>(image_size);
  if (!array) {
    LOG(ERROR) << "Error creating double-array image.";
    return false;
  }
  std::memcpy(array, trie_->array(), image_size);
  metadata->double_array = array;
  metadata->double_array_size = array_size;
  // building spelling map
  if (script) {
    auto spelling_map = CreateArray<prism::SpellingMapItem>(num_spellings);
    if (!spelling_map) {
      LOG(ERROR) << "Error creating spelling map.";
      return false;
    }
    auto i = script->begin();
    auto item = spelling_map->begin();
    for (; i != script->end(); ++i, ++item) {
      size_t list_size = i->second.size();
      item->size = list_size;
      item->at = Allocate<prism::SpellingDescriptor>(list_size);
      if (!item->at) {
        LOG(ERROR) << "Error creating spelling descriptors.";
        return false;
      }
      auto desc = item->begin();
      auto write = [&](const Spelling& spelling) {
        auto id = syllable_to_id.find(spelling.str);
        if (id == syllable_to_id.end())
          return false;
        desc->syllable_id = id->second;
        uint32_t flags = static_cast<uint32_t>(spelling.properties.type) |
                         (spelling.properties.fuzzy_mask << kFuzzyShift);
        if (spelling.properties.is_correction)
          flags |= kTypeIsCorrectionMask;
        desc->flags = flags;
        desc->credibility = spelling.properties.credibility;
        const string& tips = spelling.properties.tips;
        if (!tips.empty() && !CopyString(tips, &desc->tips)) {
          LOG(ERROR) << "Error creating spelling properties.";
          return false;
        }
        ++desc;
        return true;
      };
      if (conditional && list_size > 1) {
        // Preserve first-occurrence order while grouping alternative paths.
        map<string, size_t> positions;
        vector<vector<const Spelling*>> groups;
        for (const auto& spelling : i->second) {
          auto inserted = positions.emplace(spelling.str, groups.size());
          if (inserted.second)
            groups.emplace_back();
          groups[inserted.first->second].push_back(&spelling);
        }
        for (const auto& group : groups) {
          for (const auto* spelling : group) {
            if (!write(*spelling))
              return false;
          }
        }
      } else {
        for (const auto& spelling : i->second) {
          if (!write(spelling))
            return false;
        }
      }
    }
    metadata->spelling_map = spelling_map;
    spelling_map_ = spelling_map;
  }
  if (!choice_data.words.empty()) {
    auto words = Allocate<uint32_t>(choice_data.words.size());
    auto strings = Allocate<char>(choice_data.strings.size());
    if (!words || !strings) {
      LOG(ERROR) << "error allocating prism code choice data.";
      return false;
    }
    std::copy(choice_data.words.begin(), choice_data.words.end(), words);
    std::copy(choice_data.strings.begin(), choice_data.strings.end(), strings);
    metadata->code_choices.size = choice_data.words.size();
    metadata->code_choices.at = words;
    metadata->code_choice_strings.size = choice_data.strings.size();
    metadata->code_choice_strings.at = strings;
    LOG(INFO) << "prism code choice index: " << choice_bytes << " bytes.";
  }
  // at last, complete the metadata
  std::strncpy(metadata->format, kPrismFormat,
               prism::Metadata::kFormatMaxLength);
  format_ = 8.0;
  return true;
}

bool Prism::GetSyllabary(Syllabary* result) const {
  if (!result || !IsOpen() || !has_syllabary())
    return false;
  for (const auto& code : metadata_->syllabary)
    result->insert(code.c_str());
  return true;
}

string Prism::GetSyllableById(SyllableId id) const {
  if (!IsOpen() || !has_syllabary() || id < 0 ||
      static_cast<size_t>(id) >= metadata_->syllabary.size)
    return {};
  return metadata_->syllabary.at[id].c_str();
}

bool Prism::HasKey(std::string_view key) {
  int value = trie_->exactMatchSearch<int>(key.data(), key.length());
  return value != -1;
}

bool Prism::GetValue(std::string_view key, int* value) const {
  int result = trie_->exactMatchSearch<int>(key.data(), key.length());
  if (result == -1) {
    return false;
  }
  *value = result;
  return true;
}

// Given a key, search all the keys in the tree that share
// a common prefix with that key.
void Prism::CommonPrefixSearch(std::string_view key, vector<Match>* result) {
  if (!result || key.empty())
    return;
  result->resize(key.length());
  size_t num_results = trie_->commonPrefixSearch(key.data(), &result->front(),
                                                 key.length(), key.length());
  result->resize(num_results);
}

void Prism::ExpandSearch(std::string_view key,
                         vector<Match>* result,
                         size_t limit,
                         uint32_t enabled_fuzzy,
                         SpellingType max_type) {
  if (!result)
    return;
  result->clear();
  const bool conditional = HasConditionalSpellings();
  auto eligible = [&](int id) {
    if (!conditional)
      return true;
    for (SpellingAccessor accessor(spelling_map_, id, enabled_fuzzy, true);
         !accessor.exhausted(); accessor.Next()) {
      if (accessor.type() <= max_type)
        return true;
    }
    return false;
  };
  size_t count = 0;
  size_t node_pos = 0;
  size_t key_pos = 0;
  const char* search_key = key.empty() ? "" : key.data();
  int ret = trie_->traverse(search_key, node_pos, key_pos, key.length());
  // key is not a valid path
  if (ret == -2)
    return;
  if (ret != -1 && eligible(ret)) {
    result->push_back(Match{ret, key_pos});
    if (limit && ++count >= limit)
      return;
  }
  const char* alphabet =
      (format_ > 1.0 - DBL_EPSILON) ? metadata_->alphabet : kDefaultAlphabet;
  std::queue<node_t> q;
  q.emplace(node_pos, key_pos);
  while (!q.empty()) {
    const node_t node = q.front();
    q.pop();
    for (const char* c = alphabet; *c; ++c) {
      size_t next_node_pos = node.node_pos;
      size_t next_key_pos = 0;
      ret = trie_->traverse(c, next_node_pos, next_key_pos, 1);
      if (ret <= -2)
        continue;
      const size_t matched_length = node.key_length + next_key_pos;
      q.emplace(next_node_pos, matched_length);
      if (ret >= 0 && eligible(ret)) {
        result->push_back(Match{ret, matched_length});
        if (limit && ++count >= limit)
          return;
      }
    }
  }
}

bool Prism::HasCodeChoices() const {
  return IsOpen() && has_code_choice_format() && metadata_ &&
         metadata_->code_choices.size > metadata_->num_spellings &&
         metadata_->code_choices.at && metadata_->code_choice_strings.at;
}

prism::CodeChoiceView Prism::QueryCodeChoices(int spelling_id) const {
  if (!HasCodeChoices() || spelling_id < 0 ||
      static_cast<uint32_t>(spelling_id) >= metadata_->num_spellings)
    return {};
  const auto& words = metadata_->code_choices;
  const uint32_t offset = words.at[spelling_id];
  if (offset < metadata_->num_spellings || offset >= words.size)
    return {};
  const uint32_t count = words.at[offset];
  if (!count || count > words.size - offset - 1)
    return {};
  return {words.at.get() + offset + 1, metadata_->code_choice_strings.at.get(),
          count};
}

prism::CodeChoiceView Prism::QueryCodeChoices(std::string_view key) const {
  int id = -1;
  if (key.empty() || !HasCodeChoices() || !GetValue(key, &id))
    return {};
  return QueryCodeChoices(id);
}

uint32_t Prism::FuzzyMask(const vector<string>& groups) const {
  uint32_t result = 0;
  if (!metadata_ || !IsOpen())
    return result;
  for (size_t i = 0; i < metadata_->fuzzy_groups.size && i < kMaxFuzzyGroups;
       ++i) {
    const auto* name = metadata_->fuzzy_groups.at[i].c_str();
    if (std::find(groups.begin(), groups.end(), name) != groups.end())
      result |= 1u << i;
  }
  return result;
}

SpellingAccessor Prism::QuerySpelling(SyllableId spelling_id,
                                      uint32_t enabled_fuzzy) {
  return SpellingAccessor(spelling_map_, spelling_id, enabled_fuzzy,
                          HasConditionalSpellings());
}

size_t Prism::array_size() const {
  return trie_->size();
}

size_t Prism::max_key_length() const {
  return metadata_ ? metadata_->max_key_length : 0;
}

uint32_t Prism::dict_file_checksum() const {
  return metadata_ ? metadata_->dict_file_checksum : 0;
}

uint32_t Prism::schema_file_checksum() const {
  return metadata_ ? metadata_->schema_file_checksum : 0;
}

}  // namespace rime
