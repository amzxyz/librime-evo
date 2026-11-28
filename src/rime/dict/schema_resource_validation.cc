// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/config.h>
#include <rime/config/resource_config.h>
#include <rime/config/runtime_settings.h>
#include <rime/algo/spelling.h>
#include <rime/dict/prism_config.h>
#include <rime/dict/schema_resource_validation.h>
#include <rime/ticket.h>

namespace rime {
namespace {

using Settings = map<string, SettingGroupConfig>;

const SettingGroupConfig* ReferencedSetting(const Settings& settings,
                                            const string& reference) {
  string group;
  if (!ParseSettingReference(reference, &group))
    return nullptr;
  auto found = settings.find(group);
  return found == settings.end() ? nullptr : &found->second;
}

bool ValidateMode(const Settings& settings,
                  const string& reference,
                  const set<string>& allowed) {
  if (reference.empty() || reference.front() != '$')
    return true;
  auto setting = ReferencedSetting(settings, reference);
  if (!setting || setting->mode != SettingGroupConfig::kSingle ||
      !setting->resource_type.empty())
    return false;
  for (const auto& choice : setting->options)
    if (!allowed.count(choice.value))
      return false;
  return true;
}

bool ValidateRewrite(Config* config, const string& ns) {
  string data;
  return !config->GetString(ns + "/data", &data) ||
         ResolveResourceReference(config, "rewrite", &data);
}

bool ValidateTranslator(Config* config,
                        const Settings& settings,
                        const string& ns) {
  PrismConfig prism;
  if (!prism.Load(config, ns))
    return false;
  string dictionary;
  config->GetString(ns + "/dictionary", &dictionary);
  if (!ResolveResourceReference(config, "dictionary", &dictionary))
    return false;
  string aux;
  if (config->GetString(ns + "/aux", &aux)) {
    if (auto setting = ReferencedSetting(settings, aux)) {
      if (setting->mode != SettingGroupConfig::kSingle ||
          setting->resource_type != "auxiliary")
        return false;
      for (const auto& choice : setting->options) {
        string name = "$" + choice.value;
        if (!ResolveResourceReference(config, "auxiliary", &name))
          return false;
      }
    } else if (!ResolveResourceReference(config, "auxiliary", &aux)) {
      return false;
    }
  }
  string fuzzy;
  if (config->GetString(ns + "/fuzzy", &fuzzy)) {
    auto setting = ReferencedSetting(settings, fuzzy);
    auto definition = FindBuildResource(config, "dictionary", dictionary);
    auto declared = definition ? definition->GetValue("fuzzy") : nullptr;
    if (!setting || setting->mode != SettingGroupConfig::kMultiple ||
        !setting->resource_type.empty() ||
        setting->options.size() > kMaxFuzzyGroups || !declared ||
        declared->str() != fuzzy)
      return false;
  }
  string mode;
  if (config->GetString(ns + "/direct_aux_mode", &mode) &&
      !ValidateMode(settings, mode, {"off", "char", "word"}))
    return false;
  if (config->GetString(ns + "/user_dict_mode", &mode) &&
      !ValidateMode(settings, mode, {"off", "auto", "manual"}))
    return false;
  string charset;
  if (config->GetString(ns + "/charset", &charset)) {
    // charset identifies a policy namespace, not a compiled resource.
    if ((!charset.empty() && charset.front() == '$') ||
        !ValidateRewrite(config, charset))
      return false;
  }
  return true;
}

// Schemas compiled directly by C++ callers may not have an engine list.
// Discover only dictionary consumers in this case; never rewrite arbitrary
// fields in the configuration tree.
bool ValidateStandalone(Config* config,
                        const Settings& settings,
                        const an<ConfigItem>& item,
                        const string& path) {
  auto map = As<ConfigMap>(item);
  if (!map)
    return true;
  if ((map->GetValue("dictionary") || map->GetValue("user_dict")) &&
      !ValidateTranslator(config, settings, path)) {
    LOG(ERROR) << "invalid resource binding in " << path;
    return false;
  }
  for (const auto& entry : *map) {
    if (path.empty() &&
        (entry.first == "build" || entry.first == "runtime_settings"))
      continue;
    const string child = path.empty() ? entry.first : path + "/" + entry.first;
    if (!ValidateStandalone(config, settings, entry.second, child))
      return false;
  }
  return true;
}

string Prescription(const an<ConfigItem>& item) {
  auto value = As<ConfigValue>(item);
  if (!value) {
    auto map = As<ConfigMap>(item);
    value = map ? map->GetValue("name") : nullptr;
  }
  return value ? value->str() : string();
}

}  // namespace

bool ValidateSchemaResources(Config* config) {
  Settings settings;
  if (!SettingGroupConfig::LoadGroups(config, &settings))
    return false;
  auto translators = config->GetList("engine/translators");
  if (translators) {
    for (const auto& entry : *translators) {
      Ticket ticket(nullptr, "translator", Prescription(entry));
      if (ticket.klass != "script_translator" &&
          ticket.klass != "table_translator" &&
          ticket.klass != "reverse_lookup_translator")
        continue;
      if (!ValidateTranslator(config, settings, ticket.name_space)) {
        LOG(ERROR) << "invalid resource binding in " << ticket.name_space;
        return false;
      }
    }
  } else if (!ValidateStandalone(config, settings, config->GetItem(""), "")) {
    return false;
  }
  if (auto filters = config->GetList("engine/filters")) {
    for (const auto& entry : *filters) {
      Ticket ticket(nullptr, "filter", Prescription(entry));
      if (ticket.klass == "rewriter") {
        const string ns =
            ticket.name_space == "filter" ? "rewriter" : ticket.name_space;
        if (!ValidateRewrite(config, ns))
          return false;
      }
    }
  }
  return true;
}

}  // namespace rime
