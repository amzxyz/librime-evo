// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_PRISM_CONFIG_H_
#define RIME_PRISM_CONFIG_H_

#include <rime/common.h>

namespace rime {

class Config;

struct PrismConfig {
  struct Selection {
    string name;
    string option;
    string label{};
  };

  RIME_DLL bool Load(Config* schema, const string& name_space);
  RIME_DLL static bool ValidateReferences(Config* config);

  string name;
  vector<Selection> selections;
  bool follows_input_types = false;
};

struct SettingGroupConfig {
  enum Mode { kBoolean = 0, kSingle = 1, kMultiple = 2 };

  RIME_DLL bool Load(Config* config, const string& group = "input_types");
  RIME_DLL static bool LoadGroups(Config* config,
                                  map<string, SettingGroupConfig>* groups);
  RIME_DLL bool Contains(const string& value) const;
  RIME_DLL bool Normalize(vector<string>* values) const;
  RIME_DLL vector<string> RestoreValues(Config* user,
                                        const string& schema,
                                        const string& group) const;
  RIME_DLL bool SaveValues(Config* user,
                           const string& schema,
                           const string& group,
                           vector<string> values) const;

  Mode mode = kSingle;
  vector<PrismConfig::Selection> options;
  vector<string> defaults;
};

}  // namespace rime

#endif  // RIME_PRISM_CONFIG_H_
