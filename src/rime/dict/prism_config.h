// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_PRISM_CONFIG_H_
#define RIME_PRISM_CONFIG_H_

#include <rime/common.h>

namespace rime {

class Config;
class Context;

struct PrismConfig {
  struct Selection {
    string name;
    string option;
  };

  RIME_DLL bool Load(Config* schema, const string& name_space);
  RIME_DLL static bool ValidateReferences(Config* config);

  string name;
  vector<Selection> selections;
  bool follows_input_types = false;
};

struct OptionGroupConfig {
  RIME_DLL bool Load(Config* config, const string& group = "input_types");
  RIME_DLL static bool LoadGroups(Config* config,
                                  map<string, OptionGroupConfig>* groups);
  RIME_DLL bool Contains(const string& option) const;
  RIME_DLL string InitialOption(const Context* context) const;
  RIME_DLL string RestoreChoice(Config* user,
                                const string& schema,
                                const string& group) const;
  RIME_DLL static bool SaveChoice(Config* user,
                                  const string& schema,
                                  const string& group,
                                  const string& option);

  vector<PrismConfig::Selection> options;
};

using InputTypesConfig = OptionGroupConfig;

}  // namespace rime

#endif  // RIME_PRISM_CONFIG_H_
