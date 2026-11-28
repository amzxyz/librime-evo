// Copyright RIME Developers
// Distributed under the BSD License
//
#include <algorithm>
#include <rime/config.h>
#include <rime/algo/spelling.h>
#include <rime/dict/prism_config.h>

namespace rime {

namespace {
string ChoicePath(const string& schema, const string& group) {
  return "var/type_groups/" + schema + "/" + group;
}

bool ValidId(const string& id) {
  return !id.empty() && id.find('/') == string::npos && id.front() != '_';
}
}  // namespace

bool SettingGroupConfig::Contains(const string& value) const {
  return std::any_of(options.begin(), options.end(),
                     [&](const auto& item) { return item.option == value; });
}

bool SettingGroupConfig::Normalize(vector<string>* values) const {
  if (!values || (mode != kMultiple && values->size() != 1))
    return false;
  set<string> unique;
  for (const auto& value : *values) {
    if (!Contains(value) || !unique.insert(value).second)
      return false;
  }
  values->clear();
  for (const auto& item : options) {
    if (unique.count(item.option))
      values->push_back(item.option);
  }
  return true;
}

vector<string> SettingGroupConfig::RestoreValues(Config* user,
                                                 const string& schema,
                                                 const string& group) const {
  if (!user)
    return defaults;
  const string path = ChoicePath(schema, group);
  vector<string> values;
  if (mode == kMultiple) {
    auto list = user->GetList(path);
    if (!list)
      return defaults;
    for (const auto& item : *list) {
      auto value = As<ConfigValue>(item);
      if (value && Contains(value->str()) &&
          std::find(values.begin(), values.end(), value->str()) == values.end())
        values.push_back(value->str());
    }
  } else if (mode == kBoolean) {
    bool value = false;
    if (!user->GetBool(path, &value))
      return defaults;
    values.push_back(value ? "true" : "false");
  } else {
    string value;
    if (!user->GetString(path, &value) || !Contains(value))
      return defaults;
    values.push_back(value);
  }
  return Normalize(&values) ? values : defaults;
}

bool SettingGroupConfig::SaveValues(Config* user,
                                    const string& schema,
                                    const string& group,
                                    vector<string> values) const {
  if (!ValidId(schema) || !ValidId(group) || !Normalize(&values))
    return false;
  if (!user)
    return false;
  const string path = ChoicePath(schema, group);
  auto previous = user->GetItem(path);
  an<ConfigItem> next;
  if (mode == kMultiple) {
    auto list = New<ConfigList>();
    for (const auto& value : values)
      list->Append(New<ConfigValue>(value));
    next = list;
  } else if (mode == kBoolean) {
    next = New<ConfigValue>(values.front() == "true");
  } else {
    next = New<ConfigValue>(values.front());
  }
  if (previous && RestoreValues(user, schema, group) == values) {
    // A missing or invalid saved value must not suppress an explicit choice.
    bool exact = false;
    if (mode == kMultiple) {
      auto list = As<ConfigList>(previous);
      exact = list && list->size() == values.size();
      for (size_t i = 0; exact && i < values.size(); ++i) {
        auto item = list->GetValueAt(i);
        exact = item && item->str() == values[i];
      }
    } else {
      auto value = As<ConfigValue>(previous);
      exact = value && value->str() == values.front();
    }
    if (exact)
      return true;
  }
  if (!user->SetItem(path, next))
    return false;
  if (user->Save())
    return true;
  user->SetItem(path, previous);
  return false;
}

bool SettingGroupConfig::Load(Config* config, const string& group) {
  options.clear();
  defaults.clear();
  mode = kSingle;
  if (!config || !ValidId(group))
    return false;
  an<ConfigList> list;
  an<ConfigItem> default_item;
  if (group == "input_types" || group == "aux_types" ||
      group == "fuzzy_types") {
    if (config->IsNull(group))
      return true;
    list = config->GetList(group);
    if (group == "fuzzy_types") {
      mode = kMultiple;
      if (list && list->empty())
        return true;
      if (!list || list->size() > kMaxFuzzyGroups)
        return false;
    }
  } else {
    auto definition = config->GetMap("runtime_settings/" + group);
    if (!definition)
      return false;
    auto kind = definition->GetValue("mode");
    if (!kind)
      return false;
    if (kind->str() == "boolean")
      mode = kBoolean;
    else if (kind->str() == "multiple")
      mode = kMultiple;
    else if (kind->str() != "single")
      return false;
    list = As<ConfigList>(definition->Get("values"));
    default_item = definition->Get("default");
    if (mode == kBoolean && !list) {
      list = New<ConfigList>();
      for (const string id : {"false", "true"}) {
        auto entry = New<ConfigMap>();
        entry->Set("id", New<ConfigValue>(id));
        list->Append(entry);
      }
    }
  }
  if (!list || list->empty()) {
    LOG(ERROR) << group << " requires a nonempty values list";
    return false;
  }
  set<string> names;
  for (const auto& item : *list) {
    auto entry = As<ConfigMap>(item);
    auto value = entry ? entry->GetValue("id") : nullptr;
    // Existing input/aux schema declarations use option as the value ID.
    if (!value && entry && (group == "input_types" || group == "aux_types"))
      value = entry->GetValue("option");
    PrismConfig::Selection selection;
    if (!value || !value->GetString(&selection.option) ||
        !ValidId(selection.option) || !names.insert(selection.option).second) {
      LOG(ERROR) << group << " requires unique nonempty value IDs";
      return false;
    }
    selection.name = selection.option;
    auto label = entry->GetValue("label");
    if (!label || !label->GetString(&selection.label) ||
        selection.label.empty())
      selection.label = selection.option;
    if (group == "input_types" && entry->HasKey("name")) {
      auto name = entry->GetValue("name");
      if (!name || !name->GetString(&selection.name) || selection.name.empty())
        return false;
    }
    options.push_back(std::move(selection));
  }
  if (mode == kBoolean &&
      (options.size() != 2 || !Contains("true") || !Contains("false")))
    return false;
  if (default_item) {
    if (mode == kMultiple) {
      auto values = As<ConfigList>(default_item);
      if (!values)
        return false;
      for (const auto& item : *values) {
        auto value = As<ConfigValue>(item);
        if (!value)
          return false;
        defaults.push_back(value->str());
      }
    } else {
      auto value = As<ConfigValue>(default_item);
      if (!value)
        return false;
      defaults.push_back(value->str());
    }
  } else if (mode == kBoolean) {
    defaults.push_back("false");
  } else if (mode == kSingle) {
    defaults.push_back(options.front().option);
  }
  return Normalize(&defaults);
}

bool SettingGroupConfig::LoadGroups(Config* config,
                                    map<string, SettingGroupConfig>* groups) {
  groups->clear();
  if (!config)
    return false;
  set<string> names{"input_types", "aux_types", "fuzzy_types"};
  if (!config->IsNull("runtime_settings")) {
    auto definitions = config->GetMap("runtime_settings");
    if (!definitions)
      return false;
    for (const auto& entry : *definitions) {
      if (!ValidId(entry.first) || !names.insert(entry.first).second)
        return false;
    }
  }
  for (const auto& group : names) {
    SettingGroupConfig setting;
    if (!setting.Load(config, group)) {
      LOG(ERROR) << "invalid runtime setting: " << group;
      return false;
    }
    if (!setting.options.empty())
      groups->emplace(group, std::move(setting));
  }
  return true;
}

bool PrismConfig::Load(Config* schema, const string& name_space) {
  name.clear();
  selections.clear();
  follows_input_types = false;
  auto settings = schema ? schema->GetMap(name_space) : nullptr;
  if (!settings)
    return true;
  if (settings->HasKey("prism") && settings->HasKey("prisms")) {
    LOG(ERROR) << name_space
               << ": prism and prisms cannot be configured together";
    return false;
  }
  if (settings->HasKey("prism")) {
    auto value = settings->GetValue("prism");
    if (!value || !value->GetString(&name)) {
      LOG(ERROR) << name_space << "/prism must be a string";
      return false;
    }
  }
  if (!settings->HasKey("prisms"))
    return true;
  if (auto reference = settings->GetValue("prisms")) {
    SettingGroupConfig types;
    if (reference->str() != "input_types" || !types.Load(schema) ||
        types.options.empty()) {
      LOG(ERROR) << name_space
                 << "/prisms requires a nonempty input_types group";
      return false;
    }
    selections = std::move(types.options);
    follows_input_types = true;
    return true;
  }
  auto list = As<ConfigList>(settings->Get("prisms"));
  if (!list || list->empty()) {
    LOG(ERROR) << name_space
               << "/prisms must be input_types or a nonempty list";
    return false;
  }
  for (const auto& item : *list) {
    auto entry = As<ConfigMap>(item);
    auto prism = entry ? entry->GetValue("name") : nullptr;
    auto option = entry ? entry->GetValue("option") : nullptr;
    Selection selection;
    if (!prism || !option || !prism->GetString(&selection.name) ||
        !option->GetString(&selection.option) || selection.name.empty() ||
        selection.option.empty()) {
      LOG(ERROR) << name_space
                 << "/prisms entries require nonempty name and option";
      return false;
    }
    selections.push_back(std::move(selection));
  }
  return true;
}

namespace {
bool ValidatePrismReferences(Config* schema,
                             const an<ConfigItem>& item,
                             const string& path) {
  if (auto map = As<ConfigMap>(item)) {
    if (map->GetValue("dictionary") || map->GetValue("user_dict")) {
      PrismConfig config;
      if (!config.Load(schema, path))
        return false;
      if (auto dictionary = map->GetValue("dictionary")) {
        auto builds = schema->GetMap("build/dictionaries");
        auto definition =
            builds ? As<ConfigMap>(builds->Get(dictionary->str())) : nullptr;
        auto algebra =
            definition ? As<ConfigMap>(definition->Get("algebra")) : nullptr;
        if (config.follows_input_types && definition) {
          const string prefix = dictionary->str() + ".";
          for (const auto& selection : config.selections) {
            string name = selection.name;
            if (name.compare(0, prefix.size(), prefix) == 0)
              name.erase(0, prefix.size());
            if (!algebra || !algebra->HasKey(name)) {
              LOG(ERROR) << path << ": missing prism algebra: " << name;
              return false;
            }
          }
        }
        if (auto aux = map->GetValue("aux")) {
          if (aux->str() == "aux_types") {
            SettingGroupConfig types;
            if (!types.Load(schema, "aux_types") || types.options.empty()) {
              LOG(ERROR) << path << "/aux requires a nonempty aux_types group";
              return false;
            }
            if (!schema->IsNull("build/auxiliary")) {
              auto rules = schema->GetMap("build/auxiliary/algebra");
              for (const auto& selection : types.options) {
                if (!rules || !rules->HasKey(selection.name)) {
                  LOG(ERROR) << "missing auxiliary algebra: " << selection.name;
                  return false;
                }
              }
            }
          }
        }
      }
    }
    for (const auto& entry : *map) {
      if (path.empty() && entry.first == "build")
        continue;
      const string child =
          path.empty() ? entry.first : path + "/" + entry.first;
      if (!ValidatePrismReferences(schema, entry.second, child))
        return false;
    }
  } else if (auto list = As<ConfigList>(item)) {
    for (const auto& entry : *list) {
      if (!ValidatePrismReferences(schema, entry, path))
        return false;
    }
  }
  return true;
}
}  // namespace

bool PrismConfig::ValidateReferences(Config* config) {
  map<string, SettingGroupConfig> groups;
  return config && SettingGroupConfig::LoadGroups(config, &groups) &&
         ValidatePrismReferences(config, config->GetItem(""), "");
}

}  // namespace rime
