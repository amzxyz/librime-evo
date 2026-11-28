// Copyright RIME Developers
// Distributed under the BSD License
//
#include <algorithm>
#include <rime/config.h>
#include <rime/config/resource_config.h>
#include <rime/config/runtime_settings.h>

namespace rime {
namespace {
string ChoicePath(const string& schema, const string& group) {
  return "var/type_groups/" + schema + "/" + group;
}

bool ValidId(const string& id) {
  return !id.empty() && id.find('/') == string::npos && id.front() != '_';
}
bool ValidSymbol(const string& name) {
  string parsed;
  return ParseResourceSymbol("$" + name, &parsed);
}
}  // namespace

bool SettingGroupConfig::Contains(const string& value) const {
  return std::any_of(options.begin(), options.end(),
                     [&](const auto& item) { return item.value == value; });
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
    if (unique.count(item.value))
      values->push_back(item.value);
  }
  return true;
}

vector<string> SettingGroupConfig::RestoreValues(Config* user,
                                                 const string& schema,
                                                 const string& group) const {
  if (fixed || !user)
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
  if (fixed || !ValidId(schema) || !ValidId(group) || !Normalize(&values))
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
  *this = SettingGroupConfig();
  if (!config || !ValidSymbol(group))
    return false;
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
  if (auto type = definition->GetValue("type")) {
    resource_type = type->str();
    if ((resource_type != "prism" && resource_type != "auxiliary") ||
        mode != kSingle)
      return false;
  }
  if (auto title = definition->GetValue("label"))
    label = title->str();

  auto parse_value = [&](const string& raw, string* value) {
    if (!resource_type.empty())
      return ParseResourceSymbol(raw, value);
    if (!ValidId(raw) || raw.front() == '$')
      return false;
    *value = raw;
    return true;
  };
  set<string> names;
  auto add_choice = [&](const string& raw, const string& title) {
    string value;
    if (!parse_value(raw, &value) || !names.insert(value).second)
      return false;
    options.push_back({value, title.empty() ? value : title});
    return true;
  };
  auto choices = definition->Get("values");
  if (auto values = As<ConfigList>(choices)) {
    for (const auto& item : *values) {
      auto entry = As<ConfigMap>(item);
      auto value = entry ? entry->GetValue("value") : nullptr;
      auto title = entry ? entry->GetValue("label") : nullptr;
      if (!value || !add_choice(value->str(), title ? title->str() : string()))
        return false;
    }
  } else if (mode == kBoolean && !choices) {
    add_choice("false", "");
    add_choice("true", "");
  } else {
    return false;
  }
  if (options.empty() ||
      (mode == kBoolean &&
       (options.size() != 2 || !Contains("true") || !Contains("false"))))
    return false;

  auto read_selection = [&](const an<ConfigItem>& item) {
    vector<string> values;
    auto append = [&](const an<ConfigItem>& node) {
      auto scalar = As<ConfigValue>(node);
      string value;
      if (!scalar || !parse_value(scalar->str(), &value))
        return false;
      values.push_back(std::move(value));
      return true;
    };
    if (mode == kMultiple) {
      auto list = As<ConfigList>(item);
      if (!list)
        return false;
      for (const auto& value : *list)
        if (!append(value))
          return false;
    } else if (!append(item)) {
      return false;
    }
    if (!Normalize(&values))
      return false;
    defaults = std::move(values);
    return true;
  };

  if (auto item = definition->Get("default")) {
    if (!read_selection(item))
      return false;
  } else if (mode == kBoolean) {
    defaults.push_back("false");
  } else if (mode == kSingle) {
    defaults.push_back(options.front().value);
  }
  if (auto item = definition->Get("fixed")) {
    fixed = true;
    if (!read_selection(item))
      return false;
  }
  return true;
}

bool SettingGroupConfig::LoadGroups(Config* config,
                                    map<string, SettingGroupConfig>* groups) {
  if (!config || !groups)
    return false;
  groups->clear();
  if (config->IsNull("runtime_settings"))
    return true;
  auto definitions = config->GetMap("runtime_settings");
  if (!definitions)
    return false;
  map<string, SettingGroupConfig> loaded;
  for (const auto& entry : *definitions) {
    SettingGroupConfig setting;
    if (!setting.Load(config, entry.first)) {
      LOG(ERROR) << "invalid runtime setting: " << entry.first;
      return false;
    }
    loaded.emplace(entry.first, std::move(setting));
  }
  *groups = std::move(loaded);
  return true;
}

}  // namespace rime
