// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_RUNTIME_SETTINGS_H_
#define RIME_RUNTIME_SETTINGS_H_

#include <rime/common.h>

namespace rime {
class Config;
struct SettingChoice {
  string value;
  string label;
};

struct SettingGroupConfig {
  enum Mode { kBoolean = 0, kSingle = 1, kMultiple = 2 };

  RIME_DLL bool Load(Config* config, const string& group);
  static const SettingGroupConfig* Find(
      Config* config,
      const string& group,
      const map<string, SettingGroupConfig>* groups,
      SettingGroupConfig* fallback);
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
  vector<SettingChoice> options;
  vector<string> defaults;
  string resource_type;
  string label;
  bool fixed = false;
};

}  // namespace rime

#endif  // RIME_RUNTIME_SETTINGS_H_
