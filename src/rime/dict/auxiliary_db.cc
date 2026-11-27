// Copyright RIME Developers
// Distributed under the BSD License
//
#include <cfloat>
#include <cstring>
#include <sstream>
#include <mutex>
#include <rime/dict/auxiliary_db.h>
#include <rime/resource.h>
#include <rime/service.h>

namespace rime {

namespace {

const char kAuxiliaryFormat[] = "Rime::Auxiliary/1.0";
const char kRecordSeparator = '\x1e';
const char kFieldSeparator = '\x1f';
const char kDisplayFlag = 'd';

static string Encode(double value) {
  std::ostringstream out;
  out.precision(17);
  out << value;
  return out.str();
}

static bool Decode(const string& value, double* result) {
  if (!result)
    return false;
  std::istringstream in(value);
  in >> *result;
  return bool(in);
}

static string EncodeCharacterRecord(const AuxiliaryCharacter& value) {
  return value.text + kFieldSeparator + std::to_string(value.type) +
         kFieldSeparator + Encode(value.credibility);
}

static string EncodeCodeRecord(const AuxiliaryCode& value) {
  string record = value.code + kFieldSeparator + std::to_string(value.type) +
                  kFieldSeparator + Encode(value.credibility);
  if (value.display) {
    record += kFieldSeparator;
    record += kDisplayFlag;
  }
  return record;
}

static vector<string> SplitRecords(const string& value) {
  vector<string> result;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t end = value.find(kRecordSeparator, start);
    const size_t length =
        end == string::npos ? value.size() - start : end - start;
    if (length)
      result.push_back(value.substr(start, length));
    if (end == string::npos)
      break;
    start = end + 1;
  }
  return result;
}

static vector<string> SplitFields(const string& value) {
  vector<string> result;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t end = value.find(kFieldSeparator, start);
    const size_t length =
        end == string::npos ? value.size() - start : end - start;
    result.push_back(value.substr(start, length));
    if (end == string::npos)
      break;
    start = end + 1;
  }
  return result;
}

}  // namespace

struct AuxiliaryDb::Metadata {
  static const int kFormatMaxLength = 32;
  char format[kFormatMaxLength];
  uint32_t checksum = 0;
  uint32_t character_key_size = 0;
  uint32_t character_value_size = 0;
  uint32_t code_key_size = 0;
  uint32_t code_value_size = 0;
  OffsetPtr<char> character_keys;
  OffsetPtr<char> character_values;
  OffsetPtr<char> code_keys;
  OffsetPtr<char> code_values;
  OffsetPtr<StringId> character_index;
  OffsetPtr<StringId> code_index;
};

AuxiliaryDb::AuxiliaryDb(const path& file_path) : MappedFile(file_path) {}

namespace {
struct AuxiliaryPool {
  std::mutex mutex;
  map<path, weak<AuxiliaryDb>> databases;
};

AuxiliaryPool& SharedAuxiliaryPool() {
  // Service can release resources during static teardown. The weak pool
  // itself is process-lifetime; its entries are cleared on every finalize.
  static auto* pool = new AuxiliaryPool;
  return *pool;
}
}  // namespace

an<AuxiliaryDb> AuxiliaryDb::OpenShared(const string& group) {
  if (group.empty())
    return nullptr;
  the<ResourceResolver> resolver(
      Service::instance().CreateDeployedResourceResolver(
          {"auxiliary_db", "", ".aux.bin"}));
  const auto file = resolver->ResolvePath(group);
  auto& pool = SharedAuxiliaryPool();
  std::lock_guard<std::mutex> lock(pool.mutex);
  auto& entry = pool.databases[file];
  auto db = entry.lock();
  if (!db) {
    db = New<AuxiliaryDb>(file);
    if (!db->Load()) {
      pool.databases.erase(file);
      return nullptr;
    }
    entry = db;
  }
  ReadOnlyResourceCollector::Record(file);
  return db;
}

void AuxiliaryDb::ReleaseShared() {
  auto& pool = SharedAuxiliaryPool();
  std::lock_guard<std::mutex> lock(pool.mutex);
  pool.databases.clear();
}

the<AuxiliaryDb> AuxiliaryDb::Open(const string& group) {
  if (group.empty())
    return nullptr;
  the<ResourceResolver> resolver(
      Service::instance().CreateDeployedResourceResolver(
          {"auxiliary_db", "", ".aux.bin"}));
  auto db = std::make_unique<AuxiliaryDb>(resolver->ResolvePath(group));
  if (!db->Load())
    return nullptr;
  return db;
}

bool AuxiliaryDb::Load() {
  if (IsOpen())
    Close();
  if (!OpenReadOnly())
    return false;
  metadata_ = Find<Metadata>(0);
  if (!metadata_ || std::strncmp(metadata_->format, kAuxiliaryFormat,
                                 sizeof(kAuxiliaryFormat)) != 0) {
    Close();
    return false;
  }
  character_keys_ = the<StringTable>(new StringTable(
      metadata_->character_keys.get(), metadata_->character_key_size));
  character_values_ = the<StringTable>(new StringTable(
      metadata_->character_values.get(), metadata_->character_value_size));
  code_keys_ = the<StringTable>(
      new StringTable(metadata_->code_keys.get(), metadata_->code_key_size));
  code_values_ = the<StringTable>(new StringTable(metadata_->code_values.get(),
                                                  metadata_->code_value_size));
  return true;
}

bool AuxiliaryDb::Build(const map<string, vector<AuxiliaryCode>>& by_character,
                        const map<string, vector<AuxiliaryCharacter>>& by_code,
                        uint32_t checksum) {
  StringTableBuilder character_keys;
  StringTableBuilder character_values;
  StringTableBuilder code_keys;
  StringTableBuilder code_values;
  vector<string> character_values_text;
  vector<string> code_values_text;
  for (const auto& item : by_character) {
    string value;
    for (size_t i = 0; i < item.second.size(); ++i) {
      if (i)
        value += kRecordSeparator;
      value += EncodeCodeRecord(item.second[i]);
    }
    character_values.Add(value);
    character_keys.Add(item.first, 1.0,
                       nullptr);  // ids are assigned after Build().
    character_values_text.push_back(std::move(value));
  }
  for (const auto& item : by_code) {
    string value;
    for (size_t i = 0; i < item.second.size(); ++i) {
      if (i)
        value += kRecordSeparator;
      value += EncodeCharacterRecord(item.second[i]);
    }
    code_values.Add(value);
    code_keys.Add(item.first, 1.0, nullptr);
    code_values_text.push_back(std::move(value));
  }
  character_keys.Build();
  character_values.Build();
  code_keys.Build();
  code_values.Build();

  const size_t capacity =
      sizeof(Metadata) +
      (character_values_text.size() + code_values_text.size()) *
          sizeof(StringId) +
      character_keys.BinarySize() + character_values.BinarySize() +
      code_keys.BinarySize() + code_values.BinarySize() + 1024;
  if (!Create(capacity))
    return false;
  metadata_ = Allocate<Metadata>();
  if (!metadata_)
    return false;
  std::strncpy(metadata_->format, kAuxiliaryFormat, Metadata::kFormatMaxLength);
  metadata_->checksum = checksum;

  auto copy_table = [this](StringTableBuilder& source, OffsetPtr<char>* target,
                           uint32_t* size) {
    *size = static_cast<uint32_t>(source.BinarySize());
    char* image = Allocate<char>(*size);
    if (!image)
      return false;
    source.Dump(image, *size);
    *target = image;
    return true;
  };
  if (!copy_table(character_keys, &metadata_->character_keys,
                  &metadata_->character_key_size) ||
      !copy_table(character_values, &metadata_->character_values,
                  &metadata_->character_value_size) ||
      !copy_table(code_keys, &metadata_->code_keys,
                  &metadata_->code_key_size) ||
      !copy_table(code_values, &metadata_->code_values,
                  &metadata_->code_value_size))
    return false;

  metadata_->character_index = Allocate<StringId>(character_values_text.size());
  metadata_->code_index = Allocate<StringId>(code_values_text.size());
  if ((!character_values_text.empty() && !metadata_->character_index) ||
      (!code_values_text.empty() && !metadata_->code_index))
    return false;
  size_t i = 0;
  for (const auto& item : by_character) {
    const auto id = character_keys.Lookup(item.first);
    if (id == kInvalidStringId)
      return false;
    metadata_->character_index[id] =
        character_values.Lookup(character_values_text[i++]);
  }
  i = 0;
  for (const auto& item : by_code) {
    const auto id = code_keys.Lookup(item.first);
    if (id == kInvalidStringId)
      return false;
    metadata_->code_index[id] = code_values.Lookup(code_values_text[i++]);
  }
  return true;
}

bool AuxiliaryDb::Save() {
  return ShrinkToFit();
}

bool AuxiliaryDb::HasCode(std::string_view code) const {
  return !code.empty() && IsOpen() && code_keys_ &&
         code_keys_->LookupView(code) != kInvalidStringId;
}

bool AuxiliaryDb::Lookup(const string& code,
                         vector<AuxiliaryCharacter>* result) const {
  if (!result || !code_keys_ || !code_values_ || !metadata_)
    return false;
  result->clear();
  const auto id = code_keys_->Lookup(code);
  if (id == kInvalidStringId)
    return false;
  const auto value_id = metadata_->code_index[id];
  for (const auto& record : SplitRecords(code_values_->GetString(value_id))) {
    const auto fields = SplitFields(record);
    if (fields.size() != 3)
      continue;
    double credibility = 0.0;
    if (!Decode(fields[2], &credibility))
      continue;
    result->push_back({fields[0],
                       static_cast<SpellingType>(std::stoi(fields[1])),
                       credibility});
  }
  return !result->empty();
}

void AuxiliaryDb::CommonPrefixLookup(
    const string& input,
    vector<pair<size_t, vector<AuxiliaryCharacter>>>* result) const {
  if (!result || !code_keys_ || !code_values_ || !metadata_)
    return;
  result->clear();
  vector<StringId> ids;
  code_keys_->CommonPrefixMatch(input, &ids);
  for (const auto id : ids) {
    const string code = code_keys_->GetString(id);
    vector<AuxiliaryCharacter> matches;
    const auto value_id = metadata_->code_index[id];
    for (const auto& record : SplitRecords(code_values_->GetString(value_id))) {
      const auto fields = SplitFields(record);
      if (fields.size() != 3)
        continue;
      double credibility = 0.0;
      if (!Decode(fields[2], &credibility))
        continue;
      matches.push_back({fields[0],
                         static_cast<SpellingType>(std::stoi(fields[1])),
                         credibility});
    }
    if (!matches.empty())
      result->push_back({code.size(), std::move(matches)});
  }
}

string AuxiliaryDb::GetCharacterValue(const string& text) const {
  if (!IsOpen() || !character_keys_ || !character_values_ || !metadata_)
    return {};
  const auto id = character_keys_->Lookup(text);
  if (id == kInvalidStringId)
    return {};
  return character_values_->GetString(metadata_->character_index[id]);
}

bool AuxiliaryDb::ReverseLookup(const string& text,
                                vector<AuxiliaryCode>* result) const {
  if (!result)
    return false;
  result->clear();
  for (const auto& record : SplitRecords(GetCharacterValue(text))) {
    const auto fields = SplitFields(record);
    const bool display = fields.size() == 4 && fields[3].size() == 1 &&
                         fields[3][0] == kDisplayFlag;
    if (fields.size() != 3 && !display)
      continue;
    double credibility = 0.0;
    if (!Decode(fields[2], &credibility))
      continue;
    result->push_back({fields[0],
                       static_cast<SpellingType>(std::stoi(fields[1])),
                       credibility, display});
  }
  return !result->empty();
}

bool AuxiliaryDb::LookupDisplay(const string& text, string* result) const {
  if (!result)
    return false;
  result->clear();
  for (const auto& record : SplitRecords(GetCharacterValue(text))) {
    if (record.size() < 2 || record[record.size() - 2] != kFieldSeparator ||
        record.back() != kDisplayFlag)
      continue;
    if (!result->empty())
      result->push_back(' ');
    result->append(record, 0, record.find(kFieldSeparator));
  }
  return !result->empty();
}

uint32_t AuxiliaryDb::checksum() const {
  return metadata_ ? metadata_->checksum : 0;
}

}  // namespace rime
