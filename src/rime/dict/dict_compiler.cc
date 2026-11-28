//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-11-27 GONG Chen <chen.sst@gmail.com>
//
#include <filesystem>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <cctype>
#include <boost/algorithm/string.hpp>
#include <rime/algo/strings.h>
#include <utf8.h>
#include <rime/algo/algebra.h>
#include <rime/algo/calculus.h>
#include <rime/algo/utilities.h>
#include <rime/dict/auxiliary_db.h>
#include <rime/dict/compact_entries.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/dict_settings.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/entry_collector.h>
#include <rime/dict/preset_vocabulary.h>
#include <rime/dict/prism.h>
#include <rime/dict/prism_config.h>
#include <rime/dict/reverse_lookup_dictionary.h>
#include <rime/dict/table.h>
#include <rime/resource.h>
#include <rime/service.h>

namespace rime {

static bool IsAsciiPrismKey(const string& key);
static ReverseLookupTable ReadAuxiliarySources(const vector<path>& files);
static bool BuildAuxiliaryDatabase(const ReverseLookupTable& sources,
                                   an<ConfigList> algebra,
                                   const path& target,
                                   uint32_t checksum);

namespace {

bool GetCompactTextLength(std::string_view text, uint32_t* length) {
  if (text.size() > (std::numeric_limits<uint32_t>::max)())
    return false;
  *length = static_cast<uint32_t>(text.size());
  return true;
}

bool DetectAllSingleSyllable(const EntryCollector& collector) {
  for (const auto& entry : collector.entries) {
    if (!IsSingleSyllableCode(entry.code))
      return false;
  }
  return true;
}

void BuildSyllableIdMap(const Syllabary& syllabary,
                        map<string, SyllableId>* syllable_to_id) {
  SyllableId syllable_id = 0;
  for (const auto& syllable : syllabary) {
    (*syllable_to_id)[syllable] = syllable_id++;
  }
}

bool GetSyllableId(const map<string, SyllableId>& syllable_to_id,
                   const string& syllable,
                   SyllableId* id) {
  auto it = syllable_to_id.find(syllable);
  if (it == syllable_to_id.end()) {
    LOG(ERROR) << "invalid syllable in dictionary entry: " << syllable;
    return false;
  }
  *id = it->second;
  return true;
}

bool FillCompactEntries(EntryCollector& collector,
                        const map<string, SyllableId>& syllable_to_id,
                        CompactDictEntryList* compact) {
  compact->text.reserve(collector.total_text_bytes);
  compact->entries.reserve(collector.entries.size());
  // Consume collected entries from the front so the deque can release its
  // memory while the target representation grows.
  while (!collector.entries.empty()) {
    auto& entry = collector.entries.front();
    std::string_view syllable;
    if (!GetSingleSyllable(entry.code, &syllable)) {
      LOG(ERROR) << "unexpected multi-syllable code in compact build: '"
                 << entry.code << "'";
      return false;
    }
    uint32_t text_length = 0;
    if (!GetCompactTextLength(entry.text, &text_length)) {
      LOG(ERROR) << "dictionary entry text is too long: '" << entry.text << "'";
      return false;
    }
    SyllableId syllable_id = 0;
    if (!GetSyllableId(syllable_to_id, std::string(syllable), &syllable_id))
      return false;
    compact->entries.push_back(
        {compact->text.size(), text_length, syllable_id,
         log(entry.weight > 0 ? entry.weight : DBL_EPSILON)});
    compact->text.append(entry.text);
    collector.entries.pop_front();
  }
  return true;
}

bool FillVocabularyEntries(EntryCollector& collector,
                           const map<string, SyllableId>& syllable_to_id,
                           Vocabulary* vocabulary) {
  // Consume collected entries from the front so the deque can release its
  // memory while the vocabulary grows.
  while (!collector.entries.empty()) {
    auto& entry = collector.entries.front();
    RawCode raw_code;
    raw_code.FromStringView(entry.code);
    Code code;
    for (const auto& syllable : raw_code) {
      SyllableId syllable_id = 0;
      if (!GetSyllableId(syllable_to_id, syllable, &syllable_id))
        return false;
      code.push_back(syllable_id);
    }
    auto entries = vocabulary->LocateEntries(code);
    if (entries) {
      ShortDictEntry e;
      e.code.swap(code);
      e.text.swap(entry.text);
      e.weight = log(entry.weight > 0 ? entry.weight : DBL_EPSILON);
      entries->push_back(std::move(e));
    } else {
      LOG(ERROR) << "Error locating entries in vocabulary.";
    }
    collector.entries.pop_front();
  }
  return true;
}

void SortCompactEntries(DictSettings* settings,
                        size_t num_syllables,
                        CompactDictEntryList* compact) {
  std::stable_sort(
      compact->entries.begin(), compact->entries.end(),
      [&](const CompactDictEntry& a, const CompactDictEntry& b) {
        if (a.syllable_id != b.syllable_id)
          return a.syllable_id < b.syllable_id;
        if (settings->sort_order() != "original" && a.weight != b.weight)
          return a.weight > b.weight;
        return false;
      });
  compact->page_offsets.assign(num_syllables + 1, 0);
  for (const auto& entry : compact->entries)
    ++compact->page_offsets[entry.syllable_id + 1];
  for (size_t i = 1; i < compact->page_offsets.size(); ++i)
    compact->page_offsets[i] += compact->page_offsets[i - 1];
}

bool ExtractReverseEntries(const Vocabulary& vocabulary,
                           CompactDictEntryList* reverse_entries) {
  // Only root-page entries participate in reverse lookup. Reserving space
  // for every phrase in a large mixed dictionary defeats the memory savings.
  size_t entry_count = 0;
  size_t text_bytes = 0;
  for (const auto& page : vocabulary) {
    entry_count += page.second.entries.size();
    for (const auto& entry : page.second.entries)
      text_bytes += entry.text.size();
  }
  reverse_entries->entries.reserve(entry_count);
  reverse_entries->text.reserve(text_bytes);
  for (const auto& page : vocabulary) {
    for (const auto& entry : page.second.entries) {
      uint32_t text_length = 0;
      if (!GetCompactTextLength(entry.text, &text_length)) {
        LOG(ERROR) << "dictionary entry text is too long: '" << entry.text
                   << "'";
        return false;
      }
      reverse_entries->entries.push_back({reverse_entries->text.size(),
                                          text_length, page.first,
                                          entry.weight});
      reverse_entries->text.append(entry.text);
    }
  }
  return true;
}

bool BuildCompactTable(Table* table,
                       EntryCollector& collector,
                       DictSettings* settings,
                       uint32_t dict_file_checksum,
                       bool build_reverse,
                       CompactDictEntryList* reverse_entries) {
  map<string, SyllableId> syllable_to_id;
  BuildSyllableIdMap(collector.syllabary, &syllable_to_id);
  CompactDictEntryList compact_entries;
  if (!FillCompactEntries(collector, syllable_to_id, &compact_entries))
    return false;
  SortCompactEntries(settings, collector.syllabary.size(), &compact_entries);
  table->Remove();
  if (!table->Build(collector.syllabary, compact_entries, collector.num_entries,
                    dict_file_checksum) ||
      !table->Save()) {
    return false;
  }
  if (build_reverse) {
    // The reverse db does not need page offsets; drop them before moving the
    // remaining data over.
    compact_entries.page_offsets.clear();
    compact_entries.page_offsets.shrink_to_fit();
    *reverse_entries = std::move(compact_entries);
  }
  return true;
}

bool BuildVocabularyTable(Table* table,
                          EntryCollector& collector,
                          DictSettings* settings,
                          uint32_t dict_file_checksum,
                          bool build_reverse,
                          CompactDictEntryList* reverse_entries) {
  map<string, SyllableId> syllable_to_id;
  BuildSyllableIdMap(collector.syllabary, &syllable_to_id);
  Vocabulary vocabulary;
  if (!FillVocabularyEntries(collector, syllable_to_id, &vocabulary))
    return false;
  if (settings->sort_order() != "original")
    vocabulary.SortHomophones();
  table->Remove();
  const bool built =
      table->Build(collector.syllabary, vocabulary, collector.num_entries,
                   dict_file_checksum, /*release_vocabulary=*/true, [&]() {
                     return !build_reverse ||
                            ExtractReverseEntries(vocabulary, reverse_entries);
                   });
  return built && table->Save();
}

}  // namespace

DictCompiler::DictCompiler(Dictionary* dictionary)
    : dict_name_(dictionary->name()),
      packs_(dictionary->packs()),
      prism_(dictionary->prism()),
      tables_(dictionary->tables()),
      source_resolver_(
          Service::instance().CreateResourceResolver({"source_file", "", ""})),
      target_resolver_(Service::instance().CreateStagingResourceResolver(
          {"target_file", "", ""})) {}

DictCompiler::~DictCompiler() {}

static bool load_dict_settings_from_file(DictSettings* settings,
                                         const path& dict_file) {
  std::ifstream fin(dict_file.c_str());
  bool success = settings->LoadDictHeader(fin);
  fin.close();
  return success;
}

static bool get_dict_files_from_settings(vector<path>* dict_files,
                                         DictSettings& settings,
                                         ResourceResolver* source_resolver) {
  if (auto tables = settings.GetTables()) {
    for (auto it = tables->begin(); it != tables->end(); ++it) {
      string dict_name = As<ConfigValue>(*it)->str();
      auto dict_file = source_resolver->ResolvePath(dict_name + ".dict.yaml");
      if (!std::filesystem::exists(dict_file)) {
        LOG(ERROR) << "source file '" << dict_file << "' does not exist.";
        return false;
      }
      dict_files->push_back(dict_file);
    }
  }
  return true;
}

static uint32_t compute_dict_file_checksum(uint32_t initial_checksum,
                                           const vector<path>& dict_files,
                                           DictSettings& settings) {
  if (dict_files.empty()) {
    return initial_checksum;
  }
  ChecksumComputer cc(initial_checksum);
  for (const auto& file_path : dict_files) {
    cc.ProcessFile(file_path);
  }
  if (settings.use_preset_vocabulary()) {
    cc.ProcessFile(PresetVocabulary::DictFilePath(settings.vocabulary()));
  }
  return cc.Checksum();
}

uint64_t DictCompiler::EstimateMemory(const string& dict_name,
                                      bool auxiliary,
                                      const vector<string>& packs) {
  constexpr uint64_t kEstimatedBytesPerSourceByte = 32;
  the<ResourceResolver> resolver(
      Service::instance().CreateResourceResolver({"source_file", "", ""}));
  const path source =
      resolver->ResolvePath(dict_name + (auxiliary ? ".yaml" : ".dict.yaml"));
  std::ifstream stream(static_cast<const std::filesystem::path&>(source));
  DictSettings settings;
  set<path> sources{source};
  if (!auxiliary && stream && settings.LoadDictHeader(stream)) {
    if (auto tables = settings.GetTables()) {
      for (const auto& item : *tables) {
        if (auto value = As<ConfigValue>(item))
          sources.insert(resolver->ResolvePath(value->str() + ".dict.yaml"));
      }
    }
    if (settings.use_preset_vocabulary())
      sources.insert(PresetVocabulary::DictFilePath(settings.vocabulary()));
  }
  for (const auto& pack : packs)
    sources.insert(resolver->ResolvePath(pack + ".dict.yaml"));
  uint64_t size = 0;
  const auto maximum = std::numeric_limits<uint64_t>::max();
  for (const auto& file : sources) {
    std::error_code ec;
    const uint64_t bytes = std::filesystem::file_size(file, ec);
    if (!ec)
      size = bytes > maximum - size ? maximum : size + bytes;
  }
  return size > maximum / kEstimatedBytesPerSourceByte
             ? maximum
             : size * kEstimatedBytesPerSourceByte;
}

bool DictCompiler::ReadDictionarySpecs(Config* schema,
                                       vector<DictionarySpec>* definitions) {
  if (!schema || !definitions || !PrismConfig::ValidateReferences(schema))
    return false;
  definitions->clear();
  SettingGroupConfig fuzzy;
  if (!fuzzy.Load(schema, "fuzzy_types"))
    return false;
  if (schema->IsNull("build/dictionaries"))
    return true;
  auto dictionaries = schema->GetMap("build/dictionaries");
  if (!dictionaries) {
    LOG(ERROR) << "build/dictionaries must be a map keyed by dictionary name";
    return false;
  }

  hash_set<string> table_names;
  hash_set<string> prism_names;
  for (const auto& item : *dictionaries) {
    auto entry = As<ConfigMap>(item.second);
    DictionarySpec definition;
    definition.name = item.first;
    if (definition.name.empty() || !entry) {
      LOG(ERROR) << "build/dictionaries entries require a nonempty dictionary "
                    "name and a map of build settings";
      return false;
    }
    if (!table_names.insert(definition.name).second) {
      LOG(ERROR) << "duplicate table build: " << definition.name;
      return false;
    }
    if (!table_names.insert(definition.name + ".packs").second) {
      LOG(ERROR) << "dictionary conflicts with a generated packs table";
      return false;
    }
    if (auto packs_item = entry->Get("packs")) {
      auto packs = As<ConfigList>(packs_item);
      if (!packs) {
        LOG(ERROR) << "build dictionary '" << definition.name
                   << "' packs must be a list";
        return false;
      }
      hash_set<string> sources;
      for (const auto& pack_item : *packs) {
        auto pack = As<ConfigValue>(pack_item);
        if (!pack || pack->str().empty()) {
          LOG(ERROR) << "invalid pack in build dictionary '" << definition.name
                     << "'";
          return false;
        }
        if (pack->str() == definition.name ||
            !sources.insert(pack->str()).second) {
          LOG(ERROR) << "duplicate packs source: " << pack->str();
          return false;
        }
        definition.packs.push_back(pack->str());
      }
    }
    auto algebra = entry->Get("algebra");
    if (!algebra || As<ConfigList>(algebra)) {
      definition.prisms.emplace_back(definition.name, As<ConfigList>(algebra));
    } else if (auto groups = As<ConfigMap>(algebra)) {
      if (groups->empty()) {
        LOG(ERROR) << "build dictionary '" << definition.name
                   << "' algebra contains no groups";
        return false;
      }
      for (const auto& group : *groups) {
        auto rules = As<ConfigList>(group.second);
        if (group.first.empty() || !rules) {
          LOG(ERROR) << "invalid algebra group in build dictionary '"
                     << definition.name << "'";
          return false;
        }
        definition.prisms.emplace_back(definition.name + "." + group.first,
                                       rules);
      }
    } else {
      LOG(ERROR) << "build dictionary '" << definition.name
                 << "' algebra must be a list or a map of lists";
      return false;
    }
    for (const auto& prism : definition.prisms) {
      if (prism.second) {
        Calculus calculus;
        for (const auto& item : *prism.second) {
          auto value = As<ConfigValue>(item);
          if (!value)
            return false;
          const auto& rule = value->str();
          if (rule.size() > 5 && rule.compare(0, 5, "fuzzy") == 0 &&
              !std::isalpha(static_cast<unsigned char>(rule[5]))) {
            the<Calculation> calculation;
            try {
              calculation.reset(calculus.Parse(rule));
            } catch (const boost::regex_error&) {
              return false;
            }
            auto* derivation =
                dynamic_cast<FuzzyDerivation*>(calculation.get());
            if (!derivation || !fuzzy.Contains(derivation->group())) {
              LOG(ERROR) << "undeclared or invalid fuzzy group: " << rule;
              return false;
            }
          }
        }
      }
      if (!prism_names.insert(prism.first).second) {
        LOG(ERROR) << "duplicate prism build: " << prism.first;
        return false;
      }
    }
    definitions->push_back(std::move(definition));
  }

  return true;
}

bool DictCompiler::CompileDictionary(const DictionarySpec& definition,
                                     const path& schema_file,
                                     int options) {
  if (definition.name.empty() || definition.prisms.empty())
    return false;
  DictionaryComponent component;
  the<ResourceResolver> prism_resolver(
      Service::instance().CreateDeployedResourceResolver(
          {"prism", "", ".prism.bin"}));
  the<Dictionary> dictionary(component.Create(
      definition.name, definition.prisms.front().first, definition.packs));
  DictCompiler compiler(dictionary.get());
  compiler.set_options(options);
  for (const auto& prism : definition.prisms) {
    compiler.prism_specs_.push_back(
        {New<Prism>(prism_resolver->ResolvePath(prism.first)), prism.second});
  }
  return compiler.Compile(schema_file);
}

bool DictCompiler::CompileDictionaries(const path& schema_file, int options) {
  Config schema;
  if (!schema.LoadFromFile(schema_file))
    return false;
  vector<DictionarySpec> definitions;
  if (!ReadDictionarySpecs(&schema, &definitions))
    return false;
  bool success = true;
  for (const auto& definition : definitions) {
    if (!CompileDictionary(definition, schema_file, options))
      success = false;
  }
  return success;
}

bool DictCompiler::Compile(const path& schema_file) {
  LOG(INFO) << "compiling dictionary for " << schema_file;
  bool build_table_from_source = true;
  DictSettings settings;
  auto dict_file = source_resolver_->ResolvePath(dict_name_ + ".dict.yaml");
  if (!std::filesystem::exists(dict_file)) {
    LOG(ERROR) << "source file '" << dict_file << "' does not exist.";
    build_table_from_source = false;
  } else if (!load_dict_settings_from_file(&settings, dict_file)) {
    LOG(ERROR) << "failed to load settings from '" << dict_file << "'.";
    return false;
  }
  vector<path> dict_files;
  if (!get_dict_files_from_settings(&dict_files, settings,
                                    source_resolver_.get())) {
    return false;
  }
  uint32_t dict_file_checksum =
      compute_dict_file_checksum(0, dict_files, settings);
  bool rebuild_table = false;
  const auto& primary_table = tables_[0];
  if (primary_table->Exists() && primary_table->Load()) {
    if (build_table_from_source) {
      rebuild_table = primary_table->dict_file_checksum() != dict_file_checksum;
    } else {
      dict_file_checksum = primary_table->dict_file_checksum();
      LOG(INFO) << "reuse existing table: " << primary_table->file_path();
    }
    primary_table->Close();
  } else if (build_table_from_source) {
    rebuild_table = true;
  } else {
    LOG(ERROR) << "neither " << dict_name_ << ".dict.yaml nor " << dict_name_
               << ".table.bin exists.";
    return false;
  }
  the<ResourceResolver> reverse_resolver(
      Service::instance().CreateDeployedResourceResolver(
          {"reverse_db", "", ".reverse.bin"}));
  auto reverse = New<ReverseDb>(reverse_resolver->ResolvePath(dict_name_));
  const bool reverse_ready = reverse->Exists() && reverse->Load();
  if (!reverse_ready || reverse->dict_file_checksum() != dict_file_checksum)
    rebuild_table = true;
  if (build_table_from_source && (options_ & kRebuildTable)) {
    rebuild_table = true;
  }
  // Encoder configuration may materialize optional nodes; hash the source
  // settings before collecting either table so cache keys stay stable.
  string settings_yaml;
  if (!packs_.empty()) {
    std::ostringstream root_settings;
    settings.SaveToStream(root_settings);
    settings_yaml = root_settings.str();
  }
  reverse->Close();
  Syllabary syllabary;
  if (rebuild_table) {
    if (!build_table_from_source) {
      LOG(ERROR) << "cannot rebuild dictionary '" << dict_name_
                 << "' without its source";
      return false;
    }
    EntryCollector collector;
    if (!BuildTable(0, collector, &settings, dict_files, dict_file_checksum))
      return false;
    reverse = New<ReverseDb>(
        target_resolver_->ResolvePath(dict_name_ + ".reverse.bin"));
    syllabary = std::move(collector.syllabary);
  } else {
    if (!primary_table->Load() || !primary_table->GetSyllabary(&syllabary))
      return false;
    primary_table->Close();
  }

  // Packs may omit phrase codes; derive them only from the primary dictionary.
  auto lookup_primary = [&](const string& word) {
    string codes;
    if (reverse->IsOpen() || reverse->Load()) {
      if (!reverse->Lookup(word + "\x1fstem", &codes))
        reverse->Lookup(word, &codes);
    }
    return codes;
  };
  bool rebuild_packs = false;
  uint32_t packs_checksum = 0;
  if (!packs_.empty()) {
    vector<path> sources;
    for (const auto& source : packs_) {
      auto file = source_resolver_->ResolvePath(source + ".dict.yaml");
      if (!std::filesystem::exists(file)) {
        LOG(ERROR) << "missing packs source: " << file;
        return false;
      }
      sources.push_back(std::move(file));
    }
    // Automatic phrase encoding depends on primary readings and stems.
    boost::crc_32_type fingerprint(dict_file_checksum);
    fingerprint.process_bytes(settings_yaml.data(), settings_yaml.size());
    packs_checksum =
        compute_dict_file_checksum(fingerprint.checksum(), sources, settings);
    auto& table = tables_[1];
    rebuild_packs =
        (options_ & kRebuildTable) || !table->Exists() || !table->Load();
    if (!rebuild_packs) {
      rebuild_packs = table->dict_file_checksum() != packs_checksum;
      table->Close();
    }
    if (rebuild_packs) {
      EntryCollector collector;
      collector.set_include_preset_phrases(false);
      collector.SetFallbackLookup(lookup_primary);
      DictSettings pack_settings;
      std::istringstream yaml(settings_yaml);
      if (!pack_settings.LoadFromStream(yaml))
        return false;
      if (!BuildTable(1, collector, &pack_settings, sources, packs_checksum))
        return false;
      syllabary.insert(collector.syllabary.begin(), collector.syllabary.end());
    } else {
      if (!table->Load() || !table->GetSyllabary(&syllabary))
        return false;
      table->Close();
    }
  } else {
    std::error_code ec;
    std::filesystem::remove(
        target_resolver_->ResolvePath(dict_name_ + ".packs.table.bin"), ec);
    if (ec) {
      LOG(ERROR) << "cannot remove unused packs table: " << ec.message();
      return false;
    }
  }

  // Prism invalidation depends on the code set, not word weights or text.
  boost::crc_32_type code_checksum;
  for (const auto& code : syllabary)
    code_checksum.process_bytes(code.c_str(), code.size() + 1);
  const bool has_packs = !packs_.empty();
  code_checksum.process_byte(has_packs ? 1 : 0);
  const auto syllabary_checksum = code_checksum.checksum();
  if (prism_specs_.empty())
    prism_specs_.push_back({prism_, nullptr});
  for (const auto& spec : prism_specs_) {
    prism_ = spec.prism;
    boost::crc_32_type checksum;
    if (spec.algebra) {
      for (const auto& item : *spec.algebra) {
        auto rule = As<ConfigValue>(item);
        if (!rule) {
          LOG(ERROR) << "algebra rules must be strings";
          return false;
        }
        checksum.process_bytes(rule->str().c_str(), rule->str().size() + 1);
      }
    }
    const uint32_t algebra_checksum = checksum.checksum();
    bool rebuild = true;
    if (prism_->Exists() && prism_->Load()) {
      rebuild = prism_->dict_file_checksum() != syllabary_checksum ||
                prism_->schema_file_checksum() != algebra_checksum ||
                !prism_->has_syllabary() || !prism_->has_code_choice_format();
      prism_->Close();
    }
    if ((rebuild || (options_ & kRebuildPrism)) &&
        !BuildPrism(spec.algebra, syllabary, has_packs, syllabary_checksum,
                    algebra_checksum))
      return false;
  }
  // done!
  return true;
}

static path relocate_target(const path& source_path,
                            ResourceResolver* target_resolver) {
  auto resource_id = source_path.filename().u8string();
  return target_resolver->ResolvePath(resource_id);
}

bool DictCompiler::BuildTable(int table_index,
                              EntryCollector& collector,
                              DictSettings* settings,
                              const vector<path>& dict_files,
                              uint32_t dict_file_checksum) {
  auto& table = tables_[table_index];
  auto target_path =
      relocate_target(table->file_path(), target_resolver_.get());
  LOG(INFO) << "building table: " << target_path;
  table = New<Table>(target_path);

  collector.Configure(settings);
  collector.Collect(dict_files);
  if (options_ & kDump) {
    path dump_path(table->file_path());
    dump_path.replace_extension(".txt");
    collector.Dump(dump_path);
  }

  const bool primary_table = (table_index == 0);
  CompactDictEntryList reverse_entries;
  const bool built =
      DetectAllSingleSyllable(collector)
          ? BuildCompactTable(table.get(), collector, settings,
                              dict_file_checksum, primary_table,
                              &reverse_entries)
          : BuildVocabularyTable(table.get(), collector, settings,
                                 dict_file_checksum, primary_table,
                                 &reverse_entries);
  if (!built)
    return false;
  if (primary_table) {
    ReverseDb reverse(
        target_resolver_->ResolvePath(dict_name_ + ".reverse.bin"));
    if (!reverse.Build(settings, collector.syllabary, reverse_entries,
                       collector.stems, dict_file_checksum) ||
        !reverse.Save())
      return false;
  }
  return true;
}

static bool IsAsciiPrismKey(const string& key) {
  if (key.empty())
    return false;
  return std::all_of(key.begin(), key.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '/';
  });
}

// Read the independent AUX dictionary. Each row is simply character<TAB>code;
// YAML headers are optional, and multiple rows may define multiple variants.
static ReverseLookupTable ReadAuxiliarySources(const vector<path>& files) {
  ReverseLookupTable auxiliary;
  for (const auto& file : files) {
    std::ifstream in(file.c_str());
    DictSettings header;
    if (!header.LoadDictHeader(in, true))
      continue;
    const int text_col = header.GetColumnIndex("text");
    const int code_col = header.GetColumnIndex("code");
    if (text_col < 0 || code_col < 0)
      continue;
    string line;
    while (std::getline(in, line)) {
      boost::algorithm::trim_right(line);
      if (line.empty() || line[0] == '#')
        continue;
      auto row = strings::split(line, "\t");
      if (static_cast<int>(row.size()) <= std::max(text_col, code_col))
        continue;
      const string& word = row[text_col];
      const string& code = row[code_col];
      auto pos = word.begin();
      if (word.empty() || code.empty())
        continue;
      utf8::next(pos, word.end());
      if (pos != word.end())
        continue;
      auxiliary[word].insert(code);
    }
  }
  return auxiliary;
}

static bool BuildAuxiliaryDatabase(const ReverseLookupTable& sources,
                                   an<ConfigList> aux_algebra,
                                   const path& target,
                                   uint32_t checksum) {
  Projection projection;
  if (!projection.Load(aux_algebra))
    return false;
  map<string, vector<AuxiliaryCode>> by_character;
  map<string, vector<AuxiliaryCharacter>> by_code;
  for (const auto& [character, raw_codes] : sources) {
    map<string, SpellingProperties> entries;
    set<string> display_codes;
    for (const auto& raw_code : raw_codes) {
      Script projected;
      projected.AddSyllable(raw_code);
      projection.Apply(&projected, &display_codes);
      for (const auto& spelling : projected) {
        if (!IsAsciiPrismKey(spelling.first))
          continue;
        for (const auto& variant : spelling.second) {
          const auto& props = variant.properties;
          if (!props.auxiliary)
            continue;
          auto [it, inserted] = entries.try_emplace(spelling.first, props);
          auto& best = it->second;
          if (!inserted && (props.type < best.type ||
                            (props.type == best.type &&
                             props.credibility > best.credibility)))
            best = props;
        }
      }
    }
    for (const auto& [code, props] : entries) {
      by_character[character].push_back({code, props.type, props.credibility,
                                         display_codes.count(code) != 0});
      by_code[code].push_back({character, props.type, props.credibility});
    }
  }
  AuxiliaryDb database(target);
  database.Remove();
  return database.Build(by_character, by_code, checksum) && database.Save();
}

bool DictCompiler::ReadAuxiliarySpecs(
    Config* schema,
    vector<pair<string, an<ConfigList>>>* groups) {
  if (!schema || !groups)
    return false;
  if (!schema->IsNull("build/auxiliary") &&
      !schema->GetMap("build/auxiliary")) {
    LOG(ERROR) << "build/auxiliary must be a map";
    return false;
  }
  groups->clear();
  if (auto rules = schema->GetList("build/auxiliary/algebra")) {
    groups->emplace_back("auto", rules);
  } else if (auto named = schema->GetMap("build/auxiliary/algebra")) {
    for (const auto& item : *named) {
      auto rules = As<ConfigList>(item.second);
      if (!rules) {
        LOG(ERROR) << "build/auxiliary/algebra group '" << item.first
                   << "' is not a list";
        return false;
      }
      groups->emplace_back(item.first, rules);
    }
  } else if (!schema->IsNull("build/auxiliary/algebra")) {
    LOG(ERROR) << "build/auxiliary/algebra must be a list or a map of lists";
    return false;
  }
  // Validate references without deriving compile tasks from consumers.
  if (!PrismConfig::ValidateReferences(schema))
    return false;
  return true;
}

bool DictCompiler::CompileAuxiliary(const path& schema_file, bool rebuild) {
  if (schema_file.empty())
    return true;
  Config schema;
  if (!schema.LoadFromFile(schema_file))
    return false;
  vector<pair<string, an<ConfigList>>> groups;
  if (!ReadAuxiliarySpecs(&schema, &groups))
    return false;
  if (groups.empty())
    return true;

  string source_name;
  if (!schema.GetString("build/auxiliary/dictionary", &source_name) ||
      source_name.empty()) {
    LOG(ERROR)
        << "build/auxiliary/dictionary is required for auxiliary algebra";
    return false;
  }
  return CompileAuxiliary(source_name, groups, rebuild);
}

bool DictCompiler::CompileAuxiliary(
    string source_name,
    const vector<pair<string, an<ConfigList>>>& groups,
    bool rebuild) {
  if (groups.empty())
    return true;
  if (source_name.empty())
    return false;
  if (!boost::iends_with(source_name, ".yaml"))
    source_name += ".yaml";
  the<ResourceResolver> source_resolver(
      Service::instance().CreateResourceResolver({"source_file", "", ""}));
  const auto source = source_resolver->ResolvePath(source_name);
  if (!std::filesystem::exists(source)) {
    LOG(ERROR) << "auxiliary source '" << source_name << "' does not exist";
    return false;
  }
  const uint32_t source_checksum = Checksum(source);
  the<ResourceResolver> target_resolver(
      Service::instance().CreateStagingResourceResolver(
          {"auxiliary_db", "", ".aux.bin"}));
  ReverseLookupTable sources;
  bool sources_loaded = false;
  for (const auto& group : groups) {
    // Each artifact depends only on the shared source and this group's rules.
    boost::crc_32_type checksum(source_checksum);
    for (const auto& item : *group.second) {
      auto rule = As<ConfigValue>(item);
      if (!rule) {
        LOG(ERROR) << "build/auxiliary/algebra group '" << group.first
                   << "' contains a non-string rule";
        return false;
      }
      checksum.process_bytes(rule->str().c_str(), rule->str().size() + 1);
    }
    const auto target = target_resolver->ResolvePath(group.first);
    AuxiliaryDb database(target);
    if (!rebuild && database.Exists() && database.Load()) {
      const bool current = database.checksum() == checksum.checksum();
      database.Close();
      if (current)
        continue;
    }
    if (!sources_loaded) {
      sources = ReadAuxiliarySources({source});
      sources_loaded = true;
    }
    if (!BuildAuxiliaryDatabase(sources, group.second, target,
                                checksum.checksum())) {
      LOG(ERROR) << "failed to compile auxiliary group '" << group.first << "'";
      return false;
    }
  }
  return true;
}

bool DictCompiler::BuildPrism(const an<ConfigList>& algebra,
                              const Syllabary& syllabary,
                              bool has_packs,
                              uint32_t dict_file_checksum,
                              uint32_t schema_file_checksum) {
  LOG(INFO) << "building prism...";
  auto target_path =
      relocate_target(prism_->file_path(), target_resolver_.get());
  prism_ = New<Prism>(target_path);

  if (syllabary.empty())
    return false;
  Script script;
  vector<string> fuzzy_groups;
  if (algebra) {
    for (const auto& item : *algebra) {
      auto formula = As<ConfigValue>(item);
      if (!formula)
        return false;
      const string& text = formula->str();
      if (text.size() > 3 && text.compare(0, 3, "aux") == 0 &&
          !std::isalpha(static_cast<unsigned char>(text[3]))) {
        LOG(ERROR) << "dictionary algebra cannot contain AUX rule: " << text;
        return false;
      }
    }
    Projection projection;
    if (!projection.Load(algebra, true)) {
      LOG(ERROR) << "failed to load spelling algebra";
      return false;
    }
    for (const auto& syllable : syllabary)
      script.AddSyllable(syllable);
    projection.Apply(&script);
    fuzzy_groups = projection.fuzzy_groups();
  }
  if ((options_ & kDump) && !script.empty()) {
    path dump_path(prism_->file_path());
    dump_path.replace_extension(".txt");
    script.Dump(dump_path);
  }
  // build .prism.bin
  {
    prism_->Remove();
    if (!prism_->Build(syllabary, script.empty() ? nullptr : &script,
                       dict_file_checksum, schema_file_checksum, has_packs,
                       fuzzy_groups) ||
        !prism_->Save()) {
      return false;
    }
  }

  return true;
}

}  // namespace rime
