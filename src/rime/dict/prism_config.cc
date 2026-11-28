// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/config.h>
#include <rime/dict/prism_config.h>

namespace rime {

bool PrismConfig::Load(Config* schema, const string& name_space) {
  name.clear();
  selections.clear();
  setting.clear();
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
    if (ParseSettingReference(name, &setting)) {
      SettingGroupConfig types;
      if (!types.Load(schema, setting) ||
          types.mode != SettingGroupConfig::kSingle ||
          types.resource_type != "prism" || types.options.empty())
        return false;
      string dictionary;
      if (!schema->GetString(name_space + "/dictionary", &dictionary) ||
          !ResolveResourceReference(schema, "dictionary", &dictionary))
        return false;
      for (const auto& choice : types.options) {
        string resource = "$" + choice.value;
        if (!ResolvePrismReference(schema, dictionary, &resource))
          return false;
        selections.push_back({resource, choice.value, choice.label});
      }
      name.clear();
    } else {
      string dictionary;
      schema->GetString(name_space + "/dictionary", &dictionary);
      if (!ResolveResourceReference(schema, "dictionary", &dictionary) ||
          !ResolvePrismReference(schema, dictionary, &name))
        return false;
    }
  }
  if (!settings->HasKey("prisms"))
    return true;
  auto list = As<ConfigList>(settings->Get("prisms"));
  if (!list || list->empty()) {
    LOG(ERROR) << name_space << "/prisms must be a nonempty list";
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
    string dictionary;
    schema->GetString(name_space + "/dictionary", &dictionary);
    if (!ResolveResourceReference(schema, "dictionary", &dictionary) ||
        !ResolvePrismReference(schema, dictionary, &selection.name))
      return false;
    selections.push_back(std::move(selection));
  }
  return true;
}

}  // namespace rime
