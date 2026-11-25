//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Each build/rewrites entry produces one independently named resource.
#include <rime/lever/rewrite_compiler.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

#include <rime/algo/algebra.h>
#include <rime/config.h>
#include <rime/deployer.h>
#include <rime/lever/rewrite_preset.h>
#include <rime/path_resolver.h>
#include <rime/dict/rewrite_store.h>

namespace rime {

namespace {

namespace fs = std::filesystem;
constexpr size_t kMaxSourceLineBytes = 16 * 1024 * 1024;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr uint64_t kStageFingerprintContractVersion = 3;
constexpr uint64_t kSecondHashSeed = 0x9e3779b97f4a7c15ULL;

bool PrepareSourceLine(string* line) {
  if (!line) {
    return false;
  }
  if (!line->empty() && line->back() == '\r') {
    line->pop_back();
  }
  if (line->size() >= 3 && static_cast<unsigned char>((*line)[0]) == 0xef &&
      static_cast<unsigned char>((*line)[1]) == 0xbb &&
      static_cast<unsigned char>((*line)[2]) == 0xbf) {
    line->erase(0, 3);
  }

  const size_t first = line->find_first_not_of(" \t");
  return first != string::npos && (*line)[first] != '#';
}

bool ParseFileLine(std::string_view source, string* key, string* value) {
  if (!key || !value) {
    return false;
  }

  const size_t tab = source.find('\t');
  if (tab == std::string_view::npos || tab == 0 || tab + 1 >= source.size()) {
    return false;
  }

  key->assign(source.substr(0, tab));
  value->assign(source.substr(tab + 1));
  return !key->empty() && !value->empty();
}

path ResolveDataFile(Deployer* deployer,
                     const string& section_name,
                     const string& file_name) {
  if (!deployer || !safe_path::ValidRelativeName(file_name)) {
    LOG(ERROR) << "rewriter@" << section_name
               << "/files contains invalid relative path '" << file_name
               << "'.";
    return {};
  }
  // Prefer user resources, then shared resources; do not allow escaping
  // either root via a symlink or an absolute/parent path.
  path resolved = safe_path::ExistingFile(deployer->user_data_dir, file_name);
  if (!resolved.empty())
    return resolved;
  resolved = safe_path::ExistingFile(deployer->shared_data_dir, file_name);
  if (!resolved.empty())
    return resolved;
  LOG(ERROR) << "rewriter@" << section_name << " source '" << file_name
             << "' is missing or outside its user/shared data roots.";
  return {};
}

void ReadStringList(Config* config, const string& key, vector<string>* values);

using RewriteSourceSection = RewriteCompiler::Stage;
using RewriteSourceBinding = RewriteCompiler::Spec;

bool ResolveRewriteSources(Config* config,
                           Deployer* deployer,
                           const vector<string>& section_names,
                           vector<RewriteSourceBinding>* bindings) {
  if (!config || !deployer || !bindings) {
    return false;
  }

  bindings->clear();
  bindings->reserve(section_names.size());
  std::unique_ptr<RewritePresetCatalog> presets;
  for (const auto& name : section_names) {
    if (!safe_path::ValidRelativeName(name, false) ||
        !config->GetMap("build/rewrites/" + name)) {
      LOG(ERROR) << "invalid rewrite resource '" << name << "'.";
      return false;
    }
    const string section_name = "build/rewrites/" + name;
    const bool has_files =
        static_cast<bool>(config->GetItem(section_name + "/files"));
    const bool has_preset =
        static_cast<bool>(config->GetItem(section_name + "/preset"));
    if (has_files == has_preset) {
      LOG(ERROR) << "rewriter@" << section_name
                 << " must configure exactly one of /files or /preset.";
      return false;
    }

    RewriteSourceBinding binding;
    binding.name = name;
    if (has_files) {
      vector<string> files;
      ReadStringList(config, section_name + "/files", &files);
      if (files.empty()) {
        LOG(ERROR) << "rewriter@" << section_name << "/files is empty.";
        return false;
      }

      RewriteSourceSection stage;
      stage.name = section_name;
      config->GetString(section_name + "/key_xlit", &stage.key_xlit);
      stage.files.reserve(files.size());
      for (const auto& file_name : files) {
        const path file_path =
            ResolveDataFile(deployer, section_name, file_name);
        if (file_path.empty()) {
          return false;
        }
        stage.files.push_back({file_name, file_path});
      }
      binding.stages.push_back(std::move(stage));
    } else {
      string preset_name;
      if (!config->GetString(section_name + "/preset", &preset_name) ||
          preset_name.empty()) {
        LOG(ERROR) << "rewriter@" << section_name
                   << "/preset must be a non-empty preset name.";
        return false;
      }

      string key_xlit;
      if (config->GetString(section_name + "/key_xlit", &key_xlit) &&
          !key_xlit.empty()) {
        LOG(ERROR) << "rewriter@" << section_name
                   << " cannot combine /preset with /key_xlit.";
        return false;
      }

      if (!presets) {
        presets = std::make_unique<RewritePresetCatalog>(
            deployer->shared_data_dir / "rewriter");
        if (!presets->Load()) {
          return false;
        }
      }
      vector<RewritePresetStage> preset_stages;
      if (!presets->Resolve(preset_name, &preset_stages)) {
        LOG(ERROR) << "rewriter@" << section_name << " cannot resolve preset '"
                   << preset_name << "'.";
        return false;
      }
      binding.stages.reserve(preset_stages.size());
      for (const auto& preset_stage : preset_stages) {
        RewriteSourceSection stage;
        stage.name = preset_stage.name;
        stage.files.reserve(preset_stage.files.size());
        for (const auto& file_path : preset_stage.files) {
          stage.files.push_back({file_path.filename().string(), file_path});
        }
        binding.stages.push_back(std::move(stage));
      }
    }

    if (binding.stages.empty()) {
      return false;
    }
    bindings->push_back(std::move(binding));
  }
  return true;
}

struct StageDescriptor {
  string key_xlit;
  vector<path> files;

  bool operator==(const StageDescriptor& other) const {
    return key_xlit == other.key_xlit && files == other.files;
  }
};

StageDescriptor DescribeStage(const RewriteSourceSection& source) {
  StageDescriptor descriptor;
  descriptor.key_xlit = source.key_xlit;
  descriptor.files.reserve(source.files.size());
  // Stage file lists are small and order is semantic. Linear dedup avoids
  // requiring a hash specialization for rime::path while preserving order.
  for (const auto& file : source.files) {
    if (std::find(descriptor.files.begin(), descriptor.files.end(),
                  file.resolved_path) == descriptor.files.end()) {
      descriptor.files.push_back(file.resolved_path);
    }
  }
  return descriptor;
}

void HashBytes(RewriteStageId* id, const char* data, size_t size) {
  if (!id) {
    return;
  }
  for (size_t i = 0; i < size; ++i) {
    const uint64_t byte = static_cast<unsigned char>(data[i]);
    id->high ^= byte;
    id->high *= kFnvPrime;
    id->low ^= byte + kSecondHashSeed + (id->low << 6) + (id->low >> 2);
    id->low *= 0x9ddfea08eb382d69ULL;
  }
}

void HashU64(RewriteStageId* id, uint64_t value) {
  char bytes[8];
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = static_cast<char>((value >> (i * 8)) & 0xff);
  }
  HashBytes(id, bytes, sizeof(bytes));
}

void HashString(RewriteStageId* id, const string& value) {
  HashU64(id, value.size());
  HashBytes(id, value.data(), value.size());
}

bool ComputeStageId(const RewriteSourceSection& source,
                    RewriteStageId* stage_id) {
  if (!stage_id) {
    return false;
  }

  RewriteStageId id{kFnvOffsetBasis, kSecondHashSeed};
  HashU64(&id, kStageFingerprintContractVersion);
  HashString(&id, source.key_xlit);

  vector<path> unique_files;
  unique_files.reserve(source.files.size());
  for (const auto& file : source.files) {
    if (std::find(unique_files.begin(), unique_files.end(),
                  file.resolved_path) == unique_files.end()) {
      unique_files.push_back(file.resolved_path);
    }
  }
  HashU64(&id, unique_files.size());

  for (const auto& file_path : unique_files) {
    std::error_code ec;
    const uint64_t file_size = fs::file_size(file_path, ec);
    if (ec) {
      LOG(ERROR) << "rewriter@" << source.name
                 << " cannot read metadata for source '" << file_path
                 << "': " << ec.message()
                 << "; check that the file still exists and is readable.";
      return false;
    }

    // Stage identity follows ordered source bytes and key_xlit semantics.
    HashU64(&id, file_size);
    std::ifstream in(static_cast<const std::filesystem::path&>(file_path),
                     std::ios::binary);
    if (!in) {
      LOG(ERROR) << "rewriter@" << source.name << " cannot open source '"
                 << file_path << "' while computing its shared-stage id.";
      return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while (in) {
      in.read(buffer.data(), buffer.size());
      const std::streamsize size = in.gcount();
      if (size > 0) {
        HashBytes(&id, buffer.data(), static_cast<size_t>(size));
      }
    }
    if (!in.eof()) {
      LOG(ERROR) << "rewriter@" << source.name
                 << " encountered an I/O error while hashing source '"
                 << file_path << "'.";
      return false;
    }
  }

  if (id.empty()) {
    id.low = 1;
  }
  *stage_id = id;
  return true;
}

bool BuildKeyXlit(const string& section_name,
                  const string& key_xlit,
                  Projection* key_xlit_projection) {
  if (!key_xlit_projection || key_xlit.empty()) {
    return false;
  }

  const size_t separator = key_xlit.find('>');
  if (separator == string::npos || separator == 0 ||
      separator + 1 >= key_xlit.size() ||
      key_xlit.find('>', separator + 1) != string::npos) {
    LOG(ERROR) << "rewriter@" << section_name << "/key_xlit='" << key_xlit
               << "' is invalid; expected exactly one non-empty 'from>to' "
                  "mapping, for example 'jys>597'.";
    return false;
  }

  const string from = key_xlit.substr(0, separator);
  const string to = key_xlit.substr(separator + 1);
  const string formula = "xlit/" + from + "/" + to + "/";
  auto rules = New<ConfigList>();
  rules->Append(New<ConfigValue>(formula));
  if (!key_xlit_projection->Load(rules)) {
    LOG(ERROR) << "rewriter@" << section_name << "/key_xlit='" << key_xlit
               << "' could not be compiled as xlit; verify that both sides "
                  "form a valid transliteration mapping.";
    return false;
  }
  return true;
}

bool LoadSourceFile(const string& section_name,
                    const path& file_path,
                    Projection* key_xlit_projection,
                    RewriteStageData* stage) {
  if (!stage) {
    return false;
  }

  std::ifstream in(static_cast<const std::filesystem::path&>(file_path),
                   std::ios::binary);
  if (!in) {
    LOG(ERROR) << "rewriter@" << section_name << " cannot open source '"
               << file_path << "'; check that the file exists and is readable.";
    return false;
  }

  string line;
  size_t line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    if (line.size() > kMaxSourceLineBytes) {
      LOG(ERROR) << "rewriter@" << section_name << " source " << file_path
                 << ':' << line_number
                 << " exceeds the 16 MiB per-line limit; split or shorten "
                    "this entry.";
      return false;
    }
    if (!PrepareSourceLine(&line)) {
      continue;
    }

    string key;
    string value;
    if (!ParseFileLine(line, &key, &value)) {
      LOG(ERROR) << "rewriter@" << section_name << " source " << file_path
                 << ':' << line_number
                 << " is malformed; expected non-empty key<TAB>value.";
      return false;
    }

    string preedit;
    if (key_xlit_projection) {
      const string source_key = key;
      key_xlit_projection->Apply(&key);
      if (key.empty()) {
        LOG(ERROR) << "rewriter@" << section_name
                   << "/key_xlit converted source key '" << source_key
                   << "' to an empty key at " << file_path << ':' << line_number
                   << "; adjust key_xlit so every source key remains "
                      "non-empty.";
        return false;
      }
      if (key != source_key) {
        preedit = source_key;
      }
    }

    stage->entries.push_back(
        {std::move(key), std::move(value), std::move(preedit)});
  }

  if (in.bad()) {
    LOG(ERROR) << "rewriter@" << section_name
               << " encountered an I/O error while reading source '"
               << file_path << "'; check file permissions and storage health.";
    return false;
  }
  return true;
}

void ReadStringList(Config* config, const string& key, vector<string>* values) {
  values->clear();
  const auto item = config->GetItem(key);
  if (auto value = As<ConfigValue>(item)) {
    if (!value->str().empty()) {
      values->push_back(value->str());
    }
    return;
  }
  if (auto list = As<ConfigList>(item)) {
    for (size_t i = 0; i < list->size(); ++i) {
      if (auto value = As<ConfigValue>(list->GetAt(i))) {
        if (!value->str().empty()) {
          values->push_back(value->str());
        }
      }
    }
  }
}

bool CompileStage(const RewriteSourceSection& source,
                  RewriteStageData* output) {
  if (!output) {
    return false;
  }

  output->entries.clear();

  Projection key_xlit_projection;
  Projection* active_key_xlit = nullptr;
  if (!source.key_xlit.empty()) {
    if (!BuildKeyXlit(source.name, source.key_xlit, &key_xlit_projection)) {
      return false;
    }
    active_key_xlit = &key_xlit_projection;
  }

  // Files listed for one source stage are merged in declared order.
  vector<path> seen_files;
  seen_files.reserve(source.files.size());
  for (const auto& file : source.files) {
    const string& file_name = file.declared_name;
    const path& file_path = file.resolved_path;
    if (std::find(seen_files.begin(), seen_files.end(), file_path) !=
        seen_files.end()) {
      LOG(WARNING) << "rewriter@" << source.name << " lists source '"
                   << file_name
                   << "' more than once; later duplicate is ignored.";
      continue;
    }
    seen_files.push_back(file_path);
    if (!LoadSourceFile(source.name, file_path, active_key_xlit, output)) {
      return false;
    }
  }

  if (output->entries.empty()) {
    LOG(ERROR) << "rewriter@" << source.name
               << " produced no usable entries; check its files for "
                  "non-empty key<TAB>value lines.";
    return false;
  }
  return true;
}

}  // namespace

bool RewriteCompiler::ReadSpecs(Config* config, vector<Spec>* specs) {
  if (!config || !deployer_ || !specs)
    return false;
  specs->clear();
  if (config->IsNull("build/rewrites"))
    return true;
  auto rewrites = config->GetMap("build/rewrites");
  if (!rewrites) {
    LOG(ERROR) << "build/rewrites must be a map keyed by resource name.";
    return false;
  }
  vector<string> names;
  for (const auto& item : *rewrites)
    names.push_back(item.first);
  if (!ResolveRewriteSources(config, deployer_, names, specs))
    return false;
  for (auto& spec : *specs) {
    uint64_t bytes = 0;
    for (const auto& stage : spec.stages) {
      const auto descriptor = DescribeStage(stage);
      const auto cached = std::find_if(
          stage_cache_.begin(), stage_cache_.end(), [&](const auto& item) {
            return item.key_xlit == descriptor.key_xlit &&
                   item.files == descriptor.files;
          });
      RewriteStageId id;
      if (cached != stage_cache_.end()) {
        id = cached->id;
      } else {
        if (!ComputeStageId(stage, &id))
          return false;
        stage_cache_.push_back({descriptor.key_xlit, descriptor.files, id});
      }
      spec.ids.push_back(id);
      for (const auto& file : descriptor.files) {
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        if (ec)
          return false;
        constexpr uint64_t limit =
            ((std::numeric_limits<uint64_t>::max)() - 1024 * 1024) / 16;
        bytes += (std::min<uint64_t>)(size, limit - bytes);
      }
    }
    spec.estimated_memory = bytes * 16 + 1024 * 1024;
  }
  return true;
}

bool RewriteCompiler::Compile(const Spec& spec, Deployer* deployer) {
  const bool rebuild = false;
  if (!deployer || !safe_path::ValidRelativeName(spec.name, false) ||
      spec.ids.size() != spec.stages.size())
    return false;
  vector<path> targets;
  auto add_target = [&](const string& name) {
    if (!safe_path::ValidRelativeName(name, false))
      return false;
    const path target = safe_path::WritableFile(deployer->staging_dir,
                                                RewriteStore::FileName(name));
    if (target.empty())
      return false;
    targets.push_back(target);
    return true;
  };
  if (!add_target(spec.name))
    return false;
  for (const auto& alias : spec.aliases) {
    if (!add_target(alias))
      return false;
  }
  auto load = [&](size_t i, RewriteStageData* data) {
    return CompileStage(spec.stages[i], data);
  };
  const vector<path> reusable =
      rebuild ? vector<path>()
              : vector<path>(targets.begin() + 1, targets.end());
  if (!RewriteStore::Build(targets.front(), spec.ids, load, rebuild, reusable))
    return false;
  bool success = true;
  for (size_t i = 1; i < targets.size(); ++i) {
    if (!RewriteStore::Build(targets[i], spec.ids, load, rebuild,
                             {targets.front()}))
      success = false;
  }
  return success;
}

}  // namespace rime
