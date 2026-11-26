//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/dict/rewrite_store.h>
#include "rewrite_internal.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <limits>
#include <tuple>

#include <rime/deployer.h>
#include <rime/path_resolver.h>
#include <rime/service.h>

namespace rime {
namespace {

using namespace rewrite_internal;
constexpr char kMagic[] = {'R', 'I', 'M', 'E', 'R', 'W', 'D', '1'};
constexpr uint32_t kVersion = 2;
constexpr uint64_t kHeaderSize = 16;
constexpr uint64_t kRecordSize = 32;
constexpr uint32_t kMaxStages = 4096;

struct StageRecord {
  RewriteStageId id;
  uint64_t offset = 0;
  uint64_t size = 0;
};

bool ReadIndex(const char* data, uint64_t size, vector<StageRecord>* records) {
  records->clear();
  if (!data || size < kHeaderSize ||
      std::memcmp(data, kMagic, sizeof(kMagic)) != 0 ||
      ReadU32(data + 8) != kVersion)
    return false;
  const uint32_t count = ReadU32(data + 12);
  if (!count || count > kMaxStages ||
      count > (size - kHeaderSize) / kRecordSize)
    return false;
  uint64_t end = kHeaderSize + count * kRecordSize;
  for (uint32_t i = 0; i < count; ++i) {
    const char* p = data + kHeaderSize + i * kRecordSize;
    StageRecord record{
        {ReadU64(p), ReadU64(p + 8)}, ReadU64(p + 16), ReadU64(p + 24)};
    if (record.id.empty() || record.size == 0 || record.offset % 8 ||
        !InRange(record.offset, record.size, size))
      return false;
    const auto previous =
        std::find_if(records->begin(), records->end(),
                     [&](const auto& item) { return item.id == record.id; });
    if (previous != records->end()) {
      if (record.offset != previous->offset || record.size != previous->size)
        return false;
    } else {
      if (record.offset < end)
        return false;
      end = record.offset + record.size;
    }
    records->push_back(record);
  }
  return end == size;
}

void WriteRecord(std::ostream* out, const StageRecord& record) {
  WriteU64(out, record.id.high);
  WriteU64(out, record.id.low);
  WriteU64(out, record.offset);
  WriteU64(out, record.size);
}

using ResourceKey = std::tuple<path, path, string>;
std::mutex& ResourceMutex() {
  static std::mutex mutex;
  return mutex;
}
map<ResourceKey, weak<RewriteResource>>& Resources() {
  static map<ResourceKey, weak<RewriteResource>> resources;
  return resources;
}

void AppendUnique(string&& value, vector<string>* values) {
  if (std::find(values->begin(), values->end(), value) == values->end())
    values->push_back(std::move(value));
}

void AppendUnique(RewriteResult&& value, vector<RewriteResult>* values) {
  const auto duplicate =
      std::find_if(values->begin(), values->end(), [&](const auto& item) {
        return item.value == value.value && item.preedit == value.preedit;
      });
  if (duplicate == values->end())
    values->push_back(std::move(value));
}

}  // namespace

string RewriteStore::FileName(const string& name) {
  return name + ".rewrite.bin";
}

an<const RewriteStore> RewriteStore::Open(const path& file_path) {
  uint64_t size = 0;
  auto mapping = RewriteStage::MapFile(file_path, &size);
  vector<StageRecord> records;
  if (!ReadIndex(mapping.get(), size, &records))
    return nullptr;
  auto store = New<RewriteStore>();
  for (size_t i = 0; i < records.size(); ++i) {
    const auto& record = records[i];
    const auto previous =
        std::find_if(records.begin(), records.begin() + i,
                     [&](const auto& item) { return item.id == record.id; });
    if (previous != records.begin() + i) {
      store->stages_.push_back(store->stages_[previous - records.begin()]);
      continue;
    }
    an<RewriteStage> stage(
        new RewriteStage(mapping, record.offset, record.size));
    if (!stage->Open())
      return nullptr;
    store->stages_.push_back(std::move(stage));
  }
  static std::atomic<uint64_t> generation{0};
  store->generation_ = ++generation;
  return store;
}

bool RewriteStore::Build(const path& file_path,
                         const vector<RewriteStageId>& ids,
                         const function<bool(size_t, RewriteStageData*)>& load,
                         bool force,
                         const vector<path>& reusable) {
  if (ids.empty() || ids.size() > kMaxStages || !load)
    return false;
  auto read = [](const path& file, an<const char>* mapping,
                 vector<StageRecord>* records) {
    uint64_t size = 0;
    *mapping = RewriteStage::MapFile(file, &size);
    if (ReadIndex(mapping->get(), size, records)) {
      set<uint64_t> checked;
      bool valid = true;
      for (const auto& record : *records) {
        if (!checked.insert(record.offset).second)
          continue;
        RewriteStage stage(*mapping, record.offset, record.size);
        if (!stage.Open()) {
          valid = false;
          break;
        }
      }
      if (valid)
        return true;
    }
    mapping->reset();
    records->clear();
    return false;
  };
  auto current = [&](const vector<StageRecord>& records) {
    return ids.size() == records.size() &&
           std::equal(ids.begin(), ids.end(), records.begin(),
                      [](const auto& id, const auto& record) {
                        return id == record.id;
                      });
  };
  an<const char> old;
  vector<StageRecord> old_records;
  if (!force && read(file_path, &old, &old_records) && current(old_records))
    return true;
  // Independently named outputs may share identical source content. Copy
  // their compiled payload, without sharing a mutable file or rebuilding it.
  for (const auto& file : reusable) {
    an<const char> mapping;
    vector<StageRecord> records;
    if (file != file_path && read(file, &mapping, &records) &&
        current(records)) {
      old = std::move(mapping);
      old_records = std::move(records);
      break;
    }
  }
  const path temporary = MakeTemporaryPath(file_path);
  TemporaryFileGuard guard(temporary);
  std::ofstream out(static_cast<const std::filesystem::path&>(temporary),
                    std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out.write(kMagic, sizeof(kMagic));
  WriteU32(&out, kVersion);
  WriteU32(&out, static_cast<uint32_t>(ids.size()));
  WriteZeros(&out, ids.size() * kRecordSize);
  vector<StageRecord> records;
  records.reserve(ids.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i].empty())
      return false;
    const auto duplicate =
        std::find_if(records.begin(), records.end(),
                     [&](const auto& item) { return item.id == ids[i]; });
    if (duplicate != records.end()) {
      const StageRecord record = *duplicate;
      records.push_back(record);
      continue;
    }
    const auto position = out.tellp();
    if (position < 0)
      return false;
    const auto offset = AlignValue(static_cast<uint64_t>(position), 8);
    WriteZeros(&out, offset - static_cast<uint64_t>(position));
    const auto previous =
        std::find_if(old_records.begin(), old_records.end(),
                     [&](const auto& item) { return item.id == ids[i]; });
    uint64_t size = 0;
    if (previous != old_records.end()) {
      size = previous->size;
      const char* p = old.get() + previous->offset;
      for (uint64_t left = size; left;) {
        const auto n = (std::min)(left, uint64_t{64 * 1024});
        out.write(p, static_cast<std::streamsize>(n));
        p += n;
        left -= n;
      }
    } else {
      RewriteStageData data;
      if (!load(i, &data))
        return false;
      if (!RewriteStageBuilder().Build(&out, data))
        return false;
      const auto end = out.tellp();
      if (end < 0 || static_cast<uint64_t>(end) <= offset)
        return false;
      size = static_cast<uint64_t>(end) - offset;
    }
    if (!out)
      return false;
    records.push_back({ids[i], offset, size});
  }
  out.seekp(kHeaderSize);
  for (const auto& record : records)
    WriteRecord(&out, record);
  out.close();
  old.reset();
  std::error_code ec;
  if (!out || !FlushFileDurably(temporary) ||
      !InstallAtomically(temporary, file_path, &ec))
    return false;
  guard.Commit();
  return true;
}

bool RewriteStore::Lookup(std::string_view text, vector<string>* values) const {
  if (!values || stages_.empty())
    return false;
  values->clear();
  if (stages_.size() == 1)
    return stages_.front()->Lookup(text, values);

  vector<string> current;
  bool matched = stages_.front()->Lookup(text, &current);
  if (!matched)
    current.emplace_back(text);
  for (size_t i = 1; i < stages_.size(); ++i) {
    vector<string> next;
    next.reserve(current.size());
    vector<string> rewritten;
    for (auto& value : current) {
      if (stages_[i]->Lookup(value, &rewritten)) {
        matched = true;
        for (auto& mapped : rewritten)
          AppendUnique(std::move(mapped), &next);
      } else {
        AppendUnique(std::move(value), &next);
      }
    }
    current.swap(next);
  }
  if (matched)
    *values = std::move(current);
  return matched;
}

bool RewriteStore::LookupFirst(std::string_view text, string* value) const {
  if (!value)
    return false;
  if (stages_.size() == 1)
    return stages_.front()->LookupFirst(text, value);
  // Do not greedily choose one branch at each stage: preserve Lookup's
  // ordered, deduplicated multi-stage semantics.
  vector<string> values;
  if (!Lookup(text, &values) || values.empty()) {
    value->clear();
    return false;
  }
  *value = std::move(values.front());
  return true;
}

bool RewriteStore::LookupWithPreedit(std::string_view text,
                                     vector<RewriteResult>* values) const {
  if (!values || stages_.empty())
    return false;
  values->clear();
  if (stages_.size() == 1)
    return stages_.front()->LookupWithPreedit(text, values);

  vector<RewriteResult> current;
  bool matched = stages_.front()->LookupWithPreedit(text, &current);
  if (!matched)
    current.push_back({string(text), string()});
  for (size_t i = 1; i < stages_.size(); ++i) {
    vector<RewriteResult> next;
    next.reserve(current.size());
    vector<RewriteResult> rewritten;
    for (auto& value : current) {
      if (stages_[i]->LookupWithPreedit(value.value, &rewritten)) {
        matched = true;
        for (auto& mapped : rewritten) {
          if (!value.preedit.empty())
            mapped.preedit = value.preedit;
          AppendUnique(std::move(mapped), &next);
        }
      } else {
        AppendUnique(std::move(value), &next);
      }
    }
    current.swap(next);
  }
  if (matched)
    *values = std::move(current);
  return matched;
}

bool RewriteStore::ConvertSentence(std::string_view text,
                                   string* output,
                                   vector<RewriteSentenceState>* states) const {
  if (!output || stages_.empty())
    return false;
  bool matched = false;
  if (states) {
    states->resize(stages_.size());
    std::string_view current = text;
    for (size_t i = 0; i < stages_.size(); ++i) {
      auto& state = (*states)[i];
      if (stages_[i]->ConvertSentenceIncremental(current, &state)) {
        current = state.output;
        matched = true;
      }
    }
    if (matched)
      output->assign(current);
  } else {
    string current(text);
    for (const auto& stage : stages_) {
      string rewritten;
      if (stage->ConvertSentence(current, &rewritten)) {
        current = std::move(rewritten);
        matched = true;
      }
    }
    if (matched)
      *output = std::move(current);
  }
  return matched;
}

RewriteResource::RewriteResource(path staging, path prebuilt, string file_name)
    : staging_(std::move(staging)),
      prebuilt_(std::move(prebuilt)),
      file_name_(std::move(file_name)) {}

an<RewriteResource> RewriteResource::Require(const string& name) {
  if (!safe_path::ValidRelativeName(name, false))
    return nullptr;
  auto& deployer = Service::instance().deployer();
  const auto file_name = RewriteStore::FileName(name);
  const ResourceKey key{deployer.staging_dir, deployer.prebuilt_data_dir,
                        file_name};
  std::lock_guard<std::mutex> lock(ResourceMutex());
  auto& resources = Resources();
  for (auto it = resources.begin(); it != resources.end();) {
    if (it->second.expired())
      it = resources.erase(it);
    else
      ++it;
  }
  auto& entry = resources[key];
  if (auto resource = entry.lock())
    return resource;
  an<RewriteResource> resource(new RewriteResource(
      deployer.staging_dir, deployer.prebuilt_data_dir, file_name));
  entry = resource;
  return resource;
}

an<const RewriteStore> RewriteResource::Acquire() {
  if (auto store = std::atomic_load(&store_))
    return store;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!attempted_) {
    attempted_ = true;
    auto file = safe_path::ExistingFile(staging_, file_name_);
    if (file.empty())
      file = safe_path::ExistingFile(prebuilt_, file_name_);
    auto store = file.empty() ? nullptr : RewriteStore::Open(file);
    if (!store)
      LOG(WARNING) << "cannot load rewrite resource '" << file_name_ << "'.";
    std::atomic_store(&store_, std::move(store));
  }
  return std::atomic_load(&store_);
}

bool RewriteResource::Lookup(std::string_view text, vector<string>* values) {
  if (!values)
    return false;
  if (auto store = Acquire())
    return store->Lookup(text, values);
  values->clear();
  return false;
}

bool RewriteResource::LookupFirst(std::string_view text, string* value) {
  if (!value)
    return false;
  if (auto store = Acquire())
    return store->LookupFirst(text, value);
  value->clear();
  return false;
}

void RewriteResource::ReleaseForDeployment() {
  std::lock_guard<std::mutex> lock(ResourceMutex());
  auto& resources = Resources();
  for (auto it = resources.begin(); it != resources.end();) {
    if (auto resource = it->second.lock()) {
      std::lock_guard<std::mutex> resource_lock(resource->mutex_);
      std::atomic_store(&resource->store_, an<const RewriteStore>());
      resource->attempted_ = false;
      ++it;
    } else {
      it = resources.erase(it);
    }
  }
}

}  // namespace rime
