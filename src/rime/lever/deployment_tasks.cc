//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-12-10 GONG Chen <chen.sst@gmail.com>
//

#include <rime/build_config.h>

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <rime/common.h>
#include <rime/resource.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/setup.h>
#include <rime/algo/fs.h>
#include <rime/algo/utilities.h>
#include <rime/dict/prism_config.h>
#include <rime/lever/build_compiler.h>
#include <rime/lever/deployment_tasks.h>
#include <rime/lever/user_dict_manager.h>
#include <rime/path_resolver.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace rime {

static bool MaybeCreateDirectory(path dir);

DetectModifications::DetectModifications(TaskInitializer arg) {
  try {
    data_dirs_ = std::any_cast<vector<path>>(arg);
  } catch (const std::bad_any_cast&) {
    LOG(ERROR) << "DetectModifications: invalid arguments.";
  }
}

bool DetectModifications::Run(Deployer*) {
  time_t last_modified = 0;
  try {
    for (auto dir : data_dirs_) {
      path p = fs::canonical(dir);
      last_modified = (std::max)(last_modified,
                                 filesystem::to_time_t(fs::last_write_time(p)));
      if (fs::is_directory(p)) {
        for (fs::directory_iterator iter(p), end; iter != end; ++iter) {
          path entry(iter->path());
          if (fs::is_regular_file(fs::canonical(entry)) &&
              entry.extension().to_utf8_string() == ".yaml" &&
              entry.filename().to_utf8_string() != "user.yaml" &&
              entry.filename().to_utf8_string() != "sync.yaml") {
            last_modified =
                (std::max)(last_modified,
                           filesystem::to_time_t(fs::last_write_time(entry)));
          }
        }
      }
    }
  } catch (const fs::filesystem_error& ex) {
    LOG(ERROR) << "Error reading file information: " << ex.what();
    return true;
  }

  // TODO: store as 64-bit number to avoid the year 2038 problem
  int last_build_time = 0;
  {
    the<Config> user_config(Config::Require("user_config")->Create("user"));
    user_config->GetInt("var/last_build_time", &last_build_time);
  }
  if (last_modified > (time_t)last_build_time) {
    LOG(INFO) << "modifications detected. workspace needs update.";
    return true;
  }
  return false;
}

bool InstallationUpdate::Run(Deployer* deployer) {
  LOG(INFO) << "updating rime installation info.";
  const path& user_data_path(deployer->user_data_dir);
  if (!fs::exists(user_data_path)) {
    LOG(INFO) << "creating user data dir: " << user_data_path;
    std::error_code ec;
    if (!fs::create_directories(user_data_path, ec)) {
      LOG(ERROR) << "Error creating user data dir: " << user_data_path;
    }
  }
  path installation_info(user_data_path / "installation.yaml");
  Config config;
  string installation_id;
  string last_distro_code_name;
  string last_distro_version;
  string last_rime_version;
  if (config.LoadFromFile(installation_info) && config.IsMap("")) {
    if (config.GetString("installation_id", &installation_id)) {
      LOG(INFO) << "installation info exists. installation id: "
                << installation_id;
      // for now:
      deployer->user_id = installation_id;
    }
    string sync_dir;
    if (config.GetString("sync_dir", &sync_dir)) {
      deployer->sync_dir = path(sync_dir);
    } else {
      deployer->sync_dir = user_data_path / "sync";
    }
    LOG(INFO) << "sync dir: " << deployer->sync_dir;
    bool backup_config_files;
    if (config.GetBool("backup_config_files", &backup_config_files)) {
      deployer->backup_config_files = backup_config_files;
    }
    if (config.GetString("distribution_code_name", &last_distro_code_name)) {
      LOG(INFO) << "previous distribution: " << last_distro_code_name;
    }
    if (config.GetString("distribution_version", &last_distro_version)) {
      LOG(INFO) << "previous distribution version: " << last_distro_version;
    }
    if (config.GetString("rime_version", &last_rime_version)) {
      LOG(INFO) << "previous Rime version: " << last_rime_version;
    }
  } else {
    deployer->sync_dir = user_data_path / "sync";
  }
  if (!installation_id.empty() &&
      last_distro_code_name == deployer->distribution_code_name &&
      last_distro_version == deployer->distribution_version &&
      last_rime_version == RIME_VERSION) {
    return true;
  }
  LOG(INFO) << "creating installation info.";
  if (!config.IsMap("")) {
    config = Config();
  }
  time_t now = time(NULL);
  string time_str(ctime(&now));
  boost::trim(time_str);
  if (installation_id.empty()) {
    installation_id =
        boost::uuids::to_string(boost::uuids::random_generator()());
    LOG(INFO) << "generated installation id: " << installation_id;
    // for now:
    deployer->user_id = installation_id;
    config.SetString("installation_id", installation_id);
    config.SetString("install_time", time_str);
  } else {
    config.SetString("update_time", time_str);
  }
  if (!deployer->distribution_name.empty()) {
    config.SetString("distribution_name", deployer->distribution_name);
    LOG(INFO) << "distribution: " << deployer->distribution_name;
  }
  if (!deployer->distribution_code_name.empty()) {
    config.SetString("distribution_code_name",
                     deployer->distribution_code_name);
    LOG(INFO) << "distribution code name: " << deployer->distribution_code_name;
  }
  if (!deployer->distribution_version.empty()) {
    config.SetString("distribution_version", deployer->distribution_version);
    LOG(INFO) << "distribution version: " << deployer->distribution_version;
  }
  config.SetString("sync_dir", deployer->sync_dir.to_utf8_string());
  config.SetBool("backup_config_files", deployer->backup_config_files);
  config.SetString("rime_version", RIME_VERSION);
  LOG(INFO) << "Rime version: " << RIME_VERSION;
  return config.SaveToFile(installation_info);
}

// All schema deployment entry points share one compilation batch.
static bool DeploySchemas(Deployer* deployer,
                          DeploymentResult* result,
                          bool verbose = false) {
  if (!MaybeCreateDirectory(deployer->staging_dir)) {
    LOG(ERROR) << "cannot prepare deployment directory: "
               << deployer->staging_dir;
    return false;
  }
  BuildPlan plan(deployer, verbose, verbose);
  vector<string> prepared;
  the<ResourceResolver> resolver(Service::instance().CreateResourceResolver(
      {"schema_source_file", "", ".schema.yaml"}));
  the<ResourceResolver> compiled_resolver(
      Service::instance().CreateDeployedResourceResolver(
          {"compiled_schema", "", ".schema.yaml"}));
  for (const auto& schema_id : result->schema_ids) {
    const path source = resolver->ResolvePath(schema_id);
    if (source.empty() || !fs::exists(source)) {
      LOG(ERROR) << "missing input schema: " << schema_id;
      continue;
    }
    ConfigFileUpdate update(schema_id + ".schema.yaml", "schema/version");
    if (!update.Run(deployer)) {
      LOG(ERROR) << "schema '" << schema_id
                 << "': configuration compilation failed.";
      continue;
    }
    const path compiled = compiled_resolver->ResolvePath(schema_id);
    Config config;
    string compiled_id;
    if (!config.LoadFromFile(compiled) ||
        !config.GetString("schema/schema_id", &compiled_id) ||
        compiled_id != schema_id) {
      LOG(ERROR) << "invalid compiled schema '" << schema_id << "' at "
                 << compiled;
      continue;
    }
    if (!PrismConfig::ValidateReferences(&config)) {
      LOG(ERROR) << "schema '" << schema_id
                 << "': invalid prism or input_types configuration.";
      return false;
    }
    if (!plan.Collect(schema_id, &config, compiled)) {
      LOG(ERROR) << "schema '" << schema_id << "': build planning failed.";
      return false;
    }
    prepared.push_back(schema_id);
  }
  set<string> failed;
  if (!plan.Run(&failed)) {
    LOG(ERROR) << "resource compilation batch failed.";
    return false;
  }
  for (const auto& id : prepared) {
    if (!failed.count(id))
      result->successful_schemas.insert(id);
  }
  LOG(INFO) << "finished updating schemas: "
            << result->successful_schemas.size() << " success, "
            << result->schema_ids.size() - result->successful_schemas.size()
            << " failure.";
  return result->successful_schemas.size() == result->schema_ids.size();
}

bool WorkspaceUpdate::Run(Deployer* deployer) {
  return deployer->RunSchemaDeployment(
      [&](vector<string>* schema_ids) {
        LoadModules(kDeployerModules);
        LOG(INFO) << "updating workspace.";
        if (!ConfigFileUpdate("default.yaml", "config_version").Run(deployer)) {
          LOG(ERROR) << "workspace: default.yaml compilation failed.";
          return false;
        }
        SymlinkingPrebuiltDictionaries().Run(deployer);

        the<ResourceResolver> resolver(
            Service::instance().CreateDeployedResourceResolver(
                {"compiled_config", "", ".yaml"}));
        Config config;
        if (!config.LoadFromFile(resolver->ResolvePath("default"))) {
          LOG(ERROR) << "Error loading default config.";
          return false;
        }
        auto schema_list = config.GetList("schema_list");
        if (!schema_list) {
          LOG(WARNING) << "schema list not defined.";
          return false;
        }
        set<string> seen;
        for (const auto& item : *schema_list) {
          auto entry = As<ConfigMap>(item);
          auto schema = entry ? entry->GetValue("schema") : nullptr;
          if (schema && seen.insert(schema->str()).second)
            schema_ids->push_back(schema->str());
        }
        return true;
      },
      [&](DeploymentResult* result) {
        if (!DeploySchemas(deployer, result))
          return false;
        the<Config> user_config(Config::Require("user_config")->Create("user"));
        // TODO: store as 64-bit number to avoid the year 2038 problem
        if (!user_config ||
            !user_config->SetInt("var/last_build_time", (int)time(NULL)) ||
            !user_config->Save()) {
          LOG(ERROR) << "workspace: failed to save last build time.";
          return false;
        }
        return true;
      });
}

SchemaUpdate::SchemaUpdate(TaskInitializer arg) : verbose_(false) {
  try {
    source_path_ = std::any_cast<path>(arg);
  } catch (const std::bad_any_cast&) {
    LOG(ERROR) << "SchemaUpdate: invalid arguments.";
  }
}

static bool MaybeCreateDirectory(path dir) {
  std::error_code ec;
  if (fs::create_directories(dir, ec)) {
    return true;
  }

  if (fs::is_directory(dir, ec)) {
    return true;
  }
  LOG(ERROR) << "error creating directory '" << dir << "'.";
  return false;
}

static bool RemoveVersionSuffix(string* version, const string& suffix) {
  size_t suffix_pos = version->find(suffix);
  if (suffix_pos != string::npos) {
    version->erase(suffix_pos);
    return true;
  }
  return false;
}

static bool TrashDeprecatedUserCopy(const path& shared_copy,
                                    const path& user_copy,
                                    const string& version_key,
                                    const path& trash) {
  if (!fs::exists(shared_copy) || !fs::exists(user_copy) ||
      fs::equivalent(shared_copy, user_copy)) {
    return false;
  }
  string shared_copy_version;
  string user_copy_version;
  Config shared_config;
  if (shared_config.LoadFromFile(shared_copy)) {
    shared_config.GetString(version_key, &shared_copy_version);
    // treat "X.Y.minimal" as equal to (not greater than) "X.Y"
    // to avoid trashing the user installed full version
    RemoveVersionSuffix(&shared_copy_version, ".minimal");
  }
  Config user_config;
  bool is_customized_user_copy =
      user_config.LoadFromFile(user_copy) &&
      user_config.GetString(version_key, &user_copy_version) &&
      RemoveVersionSuffix(&user_copy_version, ".custom.");
  int cmp = CompareVersionString(shared_copy_version, user_copy_version);
  // rime-installed user copy of the same version should be kept for integrity.
  // also it could have been manually edited by user.
  if (cmp > 0 || (cmp == 0 && is_customized_user_copy)) {
    if (!MaybeCreateDirectory(trash)) {
      return false;
    }
    path backup = trash / user_copy.filename();
    std::error_code ec;
    fs::rename(user_copy, backup, ec);
    if (ec) {
      LOG(ERROR) << "error trashing file " << user_copy;
      return false;
    }
    return true;
  }
  return false;
}

bool SchemaUpdate::Run(Deployer* deployer) {
  return deployer->RunSchemaDeployment(
      [&](vector<string>* schema_ids) {
        LoadModules(kDeployerModules);
        Config config;
        string schema_id;
        if (!config.LoadFromFile(source_path_) ||
            !config.GetString("schema/schema_id", &schema_id) ||
            schema_id.empty()) {
          LOG(ERROR) << "invalid schema definition in '" << source_path_
                     << "'.";
          return false;
        }
        schema_ids->push_back(schema_id);
        return true;
      },
      [&](DeploymentResult* result) {
        return DeploySchemas(deployer, result, verbose_);
      });
}

ConfigFileUpdate::ConfigFileUpdate(TaskInitializer arg) {
  try {
    auto p = std::any_cast<pair<string, string>>(arg);
    file_name_ = p.first;
    version_key_ = p.second;
  } catch (const std::bad_any_cast&) {
    LOG(ERROR) << "ConfigFileUpdate: invalid arguments.";
  }
}

static bool ConfigNeedsUpdate(Config* config) {
  auto build_info = (*config)["__build_info"];
  if (!build_info.IsMap()) {
    LOG(INFO) << "missing build info";
    return true;
  }
  auto timestamps = build_info["timestamps"];
  if (!timestamps.IsMap()) {
    LOG(INFO) << "missing timestamps";
    return true;
  }
  the<ResourceResolver> resolver(Service::instance().CreateResourceResolver(
      {"config_source_file", "", ".yaml"}));
  for (auto entry : *timestamps.AsMap()) {
    auto value = As<ConfigValue>(entry.second);
    int recorded_time = 0;
    if (!value || !value->GetInt(&recorded_time)) {
      LOG(WARNING) << "invalid timestamp for " << entry.first;
      return true;
    }
    path source_file = resolver->ResolvePath(entry.first);
    if (!fs::exists(source_file)) {
      if (recorded_time) {
        LOG(INFO) << "source file no longer exists: " << source_file;
        return true;
      }
      continue;
    }
    if (recorded_time !=
        (int)filesystem::to_time_t(fs::last_write_time(source_file))) {
      LOG(INFO) << "source file " << (recorded_time ? "changed: " : "added: ")
                << source_file;
      return true;
    }
  }
  return false;
}

bool ConfigFileUpdate::Run(Deployer* deployer) {
  const path shared_data_path(deployer->shared_data_dir);
  const path user_data_path(deployer->user_data_dir);
  // trash deprecated user copy created by an older version of Rime
  path source_config_path(shared_data_path / file_name_);
  path dest_config_path(user_data_path / file_name_);
  path trash = user_data_path / "trash";
  if (TrashDeprecatedUserCopy(source_config_path, dest_config_path,
                              version_key_, trash)) {
    LOG(WARNING) << "deprecated user copy of '" << file_name_
                 << "' is moved to " << trash;
  }
  // build the config file if needs update
  the<ResourceResolver> resolver(
      Service::instance().CreateDeployedResourceResolver(
          {"compiled_config", "", ".yaml"}));
  Config config;
  const auto id = resolver->ToResourceId(file_name_);
  const auto compiled = resolver->ResolvePath(id);
  if (config.LoadFromFile(compiled) && !ConfigNeedsUpdate(&config))
    return true;
  if (!MaybeCreateDirectory(deployer->staging_dir)) {
    LOG(ERROR) << "cannot prepare config output directory: "
               << deployer->staging_dir;
    return false;
  }
  auto* builder = Config::Require("config_builder");
  if (!builder) {
    LOG(ERROR) << "config_builder is not registered for '" << file_name_
               << "'.";
    return false;
  }
  the<Config> built(builder->Create(file_name_));
  if (!built || !built->build_succeeded()) {
    LOG(ERROR) << "failed to compile configuration '" << file_name_ << "'.";
    return false;
  }
  return true;
}

bool PrebuildAllSchemas::Run(Deployer* deployer) {
  return deployer->RunSchemaDeployment(
      [&](vector<string>* schema_ids) {
        LoadModules(kDeployerModules);
        const path shared_data_path(deployer->shared_data_dir);
        if (!fs::is_directory(shared_data_path))
          return false;
        set<string> seen;
        for (const auto& item : fs::directory_iterator(shared_data_path)) {
          const path entry(item.path());
          if (!boost::ends_with(entry.filename().to_utf8_string(),
                                ".schema.yaml"))
            continue;
          Config config;
          string schema_id;
          if (!config.LoadFromFile(entry) ||
              !config.GetString("schema/schema_id", &schema_id) ||
              schema_id.empty()) {
            LOG(ERROR) << "invalid schema definition in '" << entry << "'.";
            return false;
          }
          if (seen.insert(schema_id).second)
            schema_ids->push_back(schema_id);
        }
        std::sort(schema_ids->begin(), schema_ids->end());
        return true;
      },
      [&](DeploymentResult* result) {
        return DeploySchemas(deployer, result);
      });
}

bool SymlinkingPrebuiltDictionaries::Run(Deployer* deployer) {
  const path shared_data_path(deployer->shared_data_dir);
  const path user_data_path(deployer->user_data_dir);
  if (!fs::exists(shared_data_path) || !fs::is_directory(shared_data_path) ||
      !fs::exists(user_data_path) || !fs::is_directory(user_data_path) ||
      fs::equivalent(shared_data_path, user_data_path))
    return false;
  bool success = false;
  // remove symlinks to shared data files created by previous version
  for (fs::directory_iterator test(user_data_path), end; test != end; ++test) {
    path entry(test->path());
    if (fs::is_symlink(entry)) {
      try {
        // a symlink becomes dangling if the target file is no longer provided
        std::error_code ec;
        auto target_path = fs::canonical(entry, ec);
        bool bad_link = bool(ec);
        bool linked_to_shared_data =
            !bad_link && target_path.has_parent_path() &&
            fs::equivalent(shared_data_path, target_path.parent_path());
        if (bad_link || linked_to_shared_data) {
          LOG(INFO) << "removing symlink: " << entry.filename();
          fs::remove(entry);
        }
      } catch (const fs::filesystem_error& ex) {
        LOG(ERROR) << entry << ": " << ex.what();
        success = false;
      }
    }
  }
  return success;
}

bool UserDictSync::Run(Deployer* deployer) {
  UserDictManager mgr(deployer);
  return mgr.SynchronizeAll();
}

bool BackupConfigFiles::Run(Deployer* deployer) {
  if (!deployer->backup_config_files) {
    LOG(INFO) << "skip backing up config files because it's disabled.";
    return true;
  }
  const path user_data_path(deployer->user_data_dir);
  std::error_code ec;
  if (!fs::exists(user_data_path, ec) || ec) {
    LOG(ERROR) << "config backup cannot access user data directory "
               << user_data_path << (ec ? ": " + ec.message() : "");
    return false;
  }
  const path device_dir(deployer->user_data_sync_dir());
  if (!MaybeCreateDirectory(device_dir)) {
    LOG(ERROR) << "config backup cannot create device directory: "
               << device_dir;
    return false;
  }
  const path backup_dir(device_dir / "backup");
  std::error_code backup_error;
  const auto backup_status = fs::symlink_status(backup_dir, backup_error);
  if (backup_error != std::errc::no_such_file_or_directory &&
      (backup_error || fs::is_symlink(backup_status))) {
    LOG(ERROR) << "config backup rejected unsafe directory: " << backup_dir
               << (backup_error ? ": " + backup_error.message() : "");
    return false;
  }
  if (!MaybeCreateDirectory(backup_dir)) {
    LOG(ERROR) << "config backup cannot create backup directory: "
               << backup_dir;
    return false;
  }

  int success = 0, failure = 0, latest = 0, skipped = 0;
  for (fs::directory_iterator iter(user_data_path, ec), end; !ec && iter != end;
       iter.increment(ec)) {
    const path entry(iter->path());
    std::error_code entry_error;
    const auto status = iter->symlink_status(entry_error);
    if (entry_error) {
      LOG(ERROR) << "config backup cannot inspect " << entry << ": "
                 << entry_error.message();
      ++failure;
      continue;
    }
    if (fs::is_symlink(status) || !fs::is_regular_file(status))
      continue;
    if (entry.filename().to_utf8_string() == "sync.yaml") {
      ++skipped;
      continue;
    }
    const auto extension = entry.extension().to_utf8_string();
    if (extension != ".yaml" && extension != ".txt")
      continue;
    const path backup = safe_path::WritableFile(
        backup_dir, entry.filename().to_utf8_string(), false);
    if (backup.empty()) {
      LOG(ERROR) << "config backup rejected unsafe target: "
                 << entry.filename();
      ++failure;
      continue;
    }
    std::error_code exists_error;
    const bool backup_exists = fs::exists(backup, exists_error);
    if (exists_error) {
      LOG(ERROR) << "config backup cannot inspect existing file " << backup
                 << ": " << exists_error.message();
      ++failure;
      continue;
    }
    if (backup_exists && Checksum(backup) == Checksum(entry)) {
      ++latest;
      continue;
    }
    std::error_code copy_error;
    fs::copy_file(entry, backup, fs::copy_options::overwrite_existing,
                  copy_error);
    if (copy_error) {
      LOG(ERROR) << "config backup failed: source=" << entry
                 << " target=" << backup << " reason=" << copy_error.message();
      ++failure;
    } else {
      ++success;
    }
  }
  if (ec) {
    LOG(ERROR) << "config backup cannot enumerate " << user_data_path << ": "
               << ec.message();
    ++failure;
  }
  if (failure)
    LOG(ERROR) << "config backup failed: path=" << backup_dir
               << " copied=" << success << " failed=" << failure
               << " unchanged=" << latest << " skipped=" << skipped;
  else
    LOG(INFO) << "config backup finished: path=" << backup_dir
              << " copied=" << success << " unchanged=" << latest
              << " skipped=" << skipped;
  return !failure;
}

bool CleanupTrash::Run(Deployer* deployer) {
  LOG(INFO) << "clean up trash.";
  const path user_data_path(deployer->user_data_dir);
  if (!fs::exists(user_data_path))
    return false;
  path trash = user_data_path / "trash";
  int success = 0, failure = 0;
  for (fs::directory_iterator iter(user_data_path), end; iter != end; ++iter) {
    path entry(iter->path());
    if (!fs::is_regular_file(entry))
      continue;
    auto file_name = entry.filename().to_utf8_string();
    if (file_name == "rime.log" || boost::ends_with(file_name, ".bin")) {
      if (!success && !MaybeCreateDirectory(trash)) {
        return false;
      }
      path backup = trash / entry.filename();
      std::error_code ec;
      fs::rename(entry, backup, ec);
      if (ec) {
        LOG(ERROR) << "error clean up file " << entry;
        ++failure;
      } else {
        ++success;
      }
    }
  }
  if (success) {
    LOG(INFO) << "moved " << success << " files to " << trash;
  }
  return !failure;
}

bool CleanOldLogFiles::Run(Deployer* deployer) {
  bool success = true;
#ifdef RIME_ENABLE_LOGGING
  if (IsLogToStderr()) {
    return success;
  }

  char ymd[12] = {0};
  time_t now = time(NULL);
  strftime(ymd, sizeof(ymd), ".%Y%m%d", localtime(&now));
  string today(ymd);
  DLOG(INFO) << "today: " << today;

  vector<string> dirs(GetLoggingDirectories());

  DLOG(INFO) << "scanning " << dirs.size() << " temp directory for log files.";

  int removed = 0;
  const string& app_name = deployer->app_name;
  for (const auto& dir : dirs) {
    // avoid iteration on non-existing directory, which may cause error
    if (!fs::exists(fs::path(dir)))
      continue;
    vector<path> files_to_remove;
    set<path> files_in_use;
    DLOG(INFO) << "temp directory: " << dir;
    try {
      // preparing files
      for (const auto& entry : fs::directory_iterator(dir)) {
        const string& file_name(path(entry.path().filename()).to_utf8_string());
        if (entry.is_regular_file() && !entry.is_symlink() &&
            boost::starts_with(file_name, app_name) &&
            boost::ends_with(file_name, ".log") &&
            !boost::contains(file_name, today)) {
          files_to_remove.push_back(entry.path());
        } else if (entry.is_symlink()) {
          auto target = fs::read_symlink(entry.path());
          const string& target_file_name(
              path(target.filename()).to_utf8_string());
          if (boost::starts_with(target_file_name, app_name) &&
              boost::ends_with(target_file_name, ".log")) {
            files_in_use.insert(target);
          }
        }
      }
    } catch (const fs::filesystem_error& ex) {
      // Catch error to skip up when we have no sufficient permissions.
      // E.g. on Android, there's no read permission on the cwd.
      LOG(WARNING) << "couldn't list directory '" << dir << "': " << ex.what();
      continue;
    }
    // remove files
    for (const auto& file : files_to_remove) {
      if (files_in_use.find(file.filename()) != files_in_use.end())
        continue;
      try {
        DLOG(INFO) << "removing log file '" << file.filename() << "'.";
        // ensure write permission
        fs::permissions(file, fs::perms::owner_write);
        fs::remove(file);
        ++removed;
      } catch (const fs::filesystem_error& ex) {
        LOG(ERROR) << ex.what();
        success = false;
      }
    }
  }
  if (removed != 0) {
    LOG(INFO) << "cleaned " << removed << " log files.";
  }
#endif  // RIME_ENABLE_LOGGING
  return success;
}

}  // namespace rime
