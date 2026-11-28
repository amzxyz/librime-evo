// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_PRISM_CONFIG_H_
#define RIME_PRISM_CONFIG_H_

#include <rime/common.h>
#include <rime/config/resource_config.h>
#include <rime/config/runtime_settings.h>

namespace rime {

class Config;

struct PrismConfig {
  struct Selection {
    string name;
    string option;
    string label{};
  };

  RIME_DLL bool Load(Config* schema, const string& name_space);
  bool Load(Config* schema,
            const string& name_space,
            const map<string, SettingGroupConfig>* groups);

  string name;
  vector<Selection> selections;
  string setting;
  string default_selection;
};

}  // namespace rime

#endif  // RIME_PRISM_CONFIG_H_
