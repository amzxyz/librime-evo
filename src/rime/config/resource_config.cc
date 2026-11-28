// Copyright RIME Developers
// Distributed under the BSD License
//
#include <algorithm>
#include <rime/config.h>
#include <rime/config/resource_config.h>

namespace rime {
namespace {
bool ValidSymbol(const string& name) {
  return !name.empty() &&
         std::all_of(name.begin(), name.end(), [](unsigned char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
         });
}
}  // namespace

bool ParseResourceSymbol(const string& text, string* name) {
  if (!name || text.size() < 2 || text.front() != '$' ||
      !ValidSymbol(text.substr(1)))
    return false;
  *name = text.substr(1);
  return true;
}

bool ParseSettingReference(const string& text, string* name) {
  if (!name || text.size() < 4 || text.compare(0, 2, "${") != 0 ||
      text.back() != '}' || !ValidSymbol(text.substr(2, text.size() - 3)))
    return false;
  *name = text.substr(2, text.size() - 3);
  return true;
}

bool ReadBuildResources(Config* config, vector<BuildResource>* resources) {
  if (!config || !resources)
    return false;
  resources->clear();
  if (config->IsNull("build"))
    return true;
  auto build = config->GetMap("build");
  if (!build)
    return false;
  for (const auto& entry : *build) {
    string name;
    if (!ParseResourceSymbol(entry.first, &name)) {
      LOG(ERROR) << "build resource must have a symbolic name: " << entry.first;
      return false;
    }
    auto definition = As<ConfigMap>(entry.second);
    auto type = definition ? definition->GetValue("type") : nullptr;
    if (!type || !ValidSymbol(type->str())) {
      LOG(ERROR) << "invalid resource declaration: " << entry.first;
      return false;
    }
    resources->push_back({name, type->str(), definition});
  }
  return true;
}

an<ConfigMap> FindBuildResource(Config* config,
                                const string& type,
                                const string& name) {
  if (!config)
    return nullptr;
  auto definition = config->GetMap("build/$" + name);
  auto kind = definition ? definition->GetValue("type") : nullptr;
  return kind && kind->str() == type ? definition : nullptr;
}

bool ResolveResourceReference(Config* config,
                              const string& type,
                              string* value) {
  if (!value || !config)
    return false;
  if (value->empty() || value->front() != '$')
    return true;
  string name;
  bool found = ParseResourceSymbol(*value, &name);
  if (found && type == "auxiliary") {
    vector<BuildResource> resources;
    found = false;
    if (!ReadBuildResources(config, &resources))
      return false;
    for (const auto& resource : resources) {
      if (resource.type != type)
        continue;
      auto groups = As<ConfigMap>(resource.definition->Get("aux"));
      if (groups && groups->HasKey("$" + name))
        found = true;
    }
  } else if (found) {
    found = bool(FindBuildResource(config, type, name));
  }
  if (!found) {
    LOG(ERROR) << "undeclared " << type << " resource: " << *value;
    return false;
  }
  *value = std::move(name);
  return true;
}

bool ResolvePrismReference(Config* config,
                           const string& dictionary,
                           string* value) {
  if (!value || !config)
    return false;
  if (value->empty() || value->front() != '$')
    return true;
  string name;
  auto definition = FindBuildResource(config, "dictionary", dictionary);
  auto groups = definition ? As<ConfigMap>(definition->Get("prism")) : nullptr;
  if (!ParseResourceSymbol(*value, &name) || !groups ||
      !groups->HasKey("$" + name)) {
    LOG(ERROR) << "undeclared prism " << *value << " in " << dictionary;
    return false;
  }
  *value = std::move(name);
  return true;
}

}  // namespace rime
