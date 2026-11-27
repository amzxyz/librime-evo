// Copyright RIME Developers
// Distributed under the BSD License
//
#include <algorithm>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/dict/prism_config.h>

namespace rime {

bool OptionGroupConfig::Contains(const string& option) const {
  return std::any_of(options.begin(), options.end(),
                     [&](const auto& item) { return item.option == option; });
}

string OptionGroupConfig::InitialOption(const Context* context) const {
  if (context) {
    for (const auto& item : options) {
      if (context->get_option(item.option))
        return item.option;
    }
  }
  return options.empty() ? string() : options.front().option;
}

namespace {
string ChoicePath(const string& schema, const string& group) {
  return "var/type_groups/" + schema + "/" + group;
}
}  // namespace

string OptionGroupConfig::RestoreChoice(Config* user,
                                        const string& schema,
                                        const string& group) const {
  string selected;
  if (user && user->GetString(ChoicePath(schema, group), &selected)) {
    if (Contains(selected))
      return selected;
    if (!selected.empty())
      return options.empty() ? string() : options.front().option;
  }
  return InitialOption(nullptr);
}

bool OptionGroupConfig::SaveChoice(Config* user,
                                   const string& schema,
                                   const string& group,
                                   const string& option) {
  if (!user || option.empty())
    return false;
  const string path = ChoicePath(schema, group);
  string previous;
  if (user->GetString(path, &previous) && previous == option)
    return true;
  return user->SetString(path, option) && user->Save();
}

bool OptionGroupConfig::Load(Config* config, const string& group) {
  options.clear();
  if (!config || config->IsNull(group))
    return true;
  auto list = config->GetList(group);
  if (!list) {
    LOG(ERROR) << group << " must be a list";
    return false;
  }
  set<string> names;
  for (const auto& item : *list) {
    auto entry = As<ConfigMap>(item);
    auto value = entry ? entry->GetValue("option") : nullptr;
    PrismConfig::Selection selection;
    if (!value || !value->GetString(&selection.option) ||
        selection.option.empty() || !names.insert(selection.option).second) {
      LOG(ERROR) << group << " requires unique nonempty option names";
      return false;
    }
    selection.name = selection.option;
    auto label = entry->GetValue("label");
    if (!label || !label->GetString(&selection.label) || selection.label.empty())
      selection.label = selection.option;
    if (group == "input_types" && entry->HasKey("name")) {
      auto name = entry->GetValue("name");
      if (!name || !name->GetString(&selection.name) ||
          selection.name.empty()) {
        LOG(ERROR) << group << " name must be a nonempty prism name";
        return false;
      }
    }
    options.push_back(std::move(selection));
  }
  return true;
}

bool OptionGroupConfig::LoadGroups(Config* config,
                                   map<string, OptionGroupConfig>* groups) {
  groups->clear();
  set<string> options;
  for (const string group : {"input_types", "aux_types"}) {
    OptionGroupConfig types;
    if (!types.Load(config, group))
      return false;
    for (const auto& item : types.options) {
      if (!options.insert(item.option).second) {
        LOG(ERROR) << "option belongs to multiple type groups: " << item.option;
        return false;
      }
    }
    if (!types.options.empty())
      groups->emplace(group, std::move(types));
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
    InputTypesConfig types;
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
            OptionGroupConfig types;
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
  map<string, OptionGroupConfig> groups;
  return config && OptionGroupConfig::LoadGroups(config, &groups) &&
         ValidatePrismReferences(config, config->GetItem(""), "");
}

}  // namespace rime
