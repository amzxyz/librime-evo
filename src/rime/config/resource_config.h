// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_RESOURCE_CONFIG_H_
#define RIME_RESOURCE_CONFIG_H_

#include <rime/common.h>

namespace rime {
class Config;
class ConfigMap;

struct BuildResource {
  string name;
  string type;
  an<ConfigMap> definition;
};

RIME_DLL bool ParseResourceSymbol(const string& text, string* name);
RIME_DLL bool ParseSettingReference(const string& text, string* name);
RIME_DLL bool ReadBuildResources(Config* config,
                                 vector<BuildResource>* resources);
RIME_DLL an<ConfigMap> FindBuildResource(Config* config,
                                         const string& type,
                                         const string& name);
// A plain name is a static resource name, possibly supplied by another schema.
// Explicit symbols must resolve in the current schema.
RIME_DLL bool ResolveResourceReference(Config* config,
                                       const string& type,
                                       string* value);
RIME_DLL bool ResolvePrismReference(Config* config,
                                    const string& dictionary,
                                    string* value);
// Resolve a static config resource using its field and declaration.
RIME_DLL bool ResolveConfigResource(Config* config,
                                    const string& path,
                                    string* value);
}  // namespace rime

#endif  // RIME_RESOURCE_CONFIG_H_
