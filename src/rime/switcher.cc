//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-12-07 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/config/runtime_settings.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/processor.h>
#include <rime/schema.h>
#include <rime/switcher.h>
#include <rime/ticket.h>
#include <rime/translation.h>
#include <rime/translator.h>

namespace rime {

Switcher::Switcher(const Ticket& ticket) : Processor(ticket) {
  context_->set_option("dumb", true);  // not going to commit anything

  // receive context notifications
  context_->select_notifier().connect([this](Context* ctx) { OnSelect(ctx); });

  user_config_.reset(Config::Require("user_config")->Create("user"));
  InitializeComponents();
  LoadSettings();
}

Switcher::~Switcher() {
  if (active_) {
    Deactivate();
  }
}

ProcessResult Switcher::ProcessKeyEvent(const KeyEvent& key_event) {
  for (const KeyEvent& hotkey : hotkeys_) {
    if (key_event == hotkey) {
      if (!active_ && engine_) {
        Activate();
      } else if (active_) {
        if (!settings_menu_)
          RefreshSettingsMenu();
      }
      return kAccepted;
    }
  }
  if (active_) {
    if (!key_event.release() && key_event.keycode() == XK_Escape) {
      Deactivate();
      return kAccepted;
    }
    for (auto& p : processors_) {
      ProcessResult result = p->ProcessKeyEvent(key_event);
      if (result != kNoop) {
        return result;
      }
    }
    if (key_event.release() || key_event.ctrl() || key_event.alt()) {
      return kAccepted;
    }
    int ch = key_event.keycode();
    if (ch == XK_space || ch == XK_Return) {
      context_->ConfirmCurrentSelection();
    }
    return kAccepted;
  }
  return kNoop;
}

namespace {
class SettingMenuCommand : public SimpleCandidate, public SwitcherCommand {
 public:
  SettingMenuCommand(const string& label,
                     const string& comment,
                     function<void()> action)
      : SimpleCandidate("setting", 0, 0, label, comment),
        SwitcherCommand(""),
        action_(std::move(action)) {}
  void Apply(Switcher*) override { action_(); }

 private:
  function<void()> action_;
};

string SettingSummary(const SettingGroupConfig& setting,
                      const Context* context,
                      const string& group) {
  string summary;
  for (const auto& option : setting.options) {
    if (!context->setting_selected(group, option.value))
      continue;
    if (!summary.empty())
      summary += " / ";
    summary += option.label;
  }
  return summary.empty() ? "—" : summary;
}
}  // namespace

void Switcher::RefreshSettingsMenu(string group,
                                   const string& error,
                                   size_t highlight) {
  if (!engine_)
    return;
  const auto& groups = engine_->context()->setting_groups();
  settings_menu_ = true;
  setting_group_ = group;
  auto translation = New<FifoTranslation>();
  auto append = [&](const string& label, const string& comment,
                    function<void()> action) {
    translation->Append(
        New<SettingMenuCommand>(label, comment, std::move(action)));
  };
  const auto found = groups.find(group);
  string prompt = "〔" + engine_->schema()->schema_name() + " · 设置〕";
  if (found == groups.end()) {
    setting_group_.clear();
    for (const auto& [id, setting] : groups) {
      auto label = setting.label.empty() ? id : setting.label;
      auto summary = SettingSummary(setting, engine_->context(), id);
      if (setting.fixed)
        summary += " · 固定";
      append(label, summary, [this, id] { RefreshSettingsMenu(id); });
    }
    if (groups.empty())
      append("无运行时设置", "", [] {});
  } else {
    const auto& setting = found->second;
    prompt = "〔" + (setting.label.empty() ? group : setting.label) + "〕";
    for (const auto& option : setting.options) {
      const bool selected =
          engine_->context()->setting_selected(group, option.value);
      string comment = selected ? "✓" : "";
      if (setting.fixed)
        comment += " 固定";
      const auto value = option.value;
      const auto mode = setting.mode;
      const bool fixed = setting.fixed;
      append(option.label, comment, [this, group, value, mode, fixed] {
        const size_t index = context_->composition().back().selected_index;
        if (fixed) {
          RefreshSettingsMenu(group, {}, index);
          return;
        }
        auto values = engine_->context()->setting_values(group);
        if (mode == SettingGroupConfig::kMultiple) {
          auto it = std::find(values.begin(), values.end(), value);
          if (it == values.end())
            values.push_back(value);
          else
            values.erase(it);
        } else {
          values = {value};
        }
        const bool ok = engine_->SelectSetting(group, std::move(values));
        if (!ok) {
          RefreshSettingsMenu(group, "设置未生效", index);
        } else if (mode == SettingGroupConfig::kMultiple) {
          RefreshSettingsMenu(group, {}, index);
        } else {
          Deactivate();
        }
      });
    }
    append("返回设置", "", [this] { RefreshSettingsMenu(); });
  }
  if (!error.empty())
    prompt += " " + error;
  auto menu = New<Menu>();
  menu->AddTranslation(translation);
  context_->Clear();
  Segment seg(0, 0);
  seg.prompt = prompt;
  seg.menu = menu;
  const auto count = menu->Prepare(highlight + 1);
  seg.selected_index = count ? std::min(highlight, count - 1) : 0;
  context_->composition().AddSegment(seg);
}

/*
  Example configuration:

  ```yaml
  schema_list:
  - case: [mode/wubi, mode/wubi_pinyin]
    schema: wubi_pinyin
  - case: [mode/wubi]
    schema: wubi86
  - case: [mode/default]
    schema: pinyin

  mode:
    wubi: false
    wubi_pinyin: false
    default: true
  ```
*/

static an<ConfigValue> ParseSchemaListEntry(Config* config,
                                            an<ConfigMap> entry_map) {
  if (!entry_map)
    return nullptr;
  auto schema_property = entry_map->GetValue("schema");
  if (!schema_property)
    return nullptr;
  if (auto case_conditions = As<ConfigList>(entry_map->Get("case"))) {
    for (auto iter = case_conditions->begin(); iter != case_conditions->end();
         ++iter) {
      if (auto condition_variable = As<ConfigValue>(*iter)) {
        bool condition_met = false;
        if (!config->GetBool(condition_variable->str(), &condition_met) ||
            !condition_met) {
          return nullptr;
        }
      }
    }
  }
  return schema_property;
}

int Switcher::ForEachSchemaListEntry(
    Config* config,
    function<bool(const string& schema_id)> process_entry) {
  auto schema_list = config->GetList("schema_list");
  if (!schema_list)
    return 0;
  int num_processed_entries = 0;
  for (auto iter = schema_list->begin(); iter != schema_list->end(); ++iter) {
    auto entry = ParseSchemaListEntry(config, As<ConfigMap>(*iter));
    if (!entry)
      continue;
    const string& schema_id = entry->str();
    ++num_processed_entries;
    if (!process_entry(schema_id))
      break;
  }
  return num_processed_entries;
}

void Switcher::SetActiveSchema(const string& schema_id) {
  if (user_config_) {
    user_config_->SetString("var/previously_selected_schema", schema_id);
    user_config_->SetInt("var/schema_access_time/" + schema_id, time(NULL));
    // persist recently used schema and options that have changed
    user_config_->Save();
  }
}

string Switcher::InitialSchemaId(Config* config, Config* user) {
  if (!config)
    return {};
  bool fixed = false;
  config->GetBool("switcher/fix_schema_list_order", &fixed);
  string previous;
  if (user && !fixed) {
    user->GetString("var/previously_selected_schema", &previous);
  }
  string recent;
  ForEachSchemaListEntry(config, [&previous, &recent](const string& schema_id) {
    if (previous.empty() || previous == schema_id) {
      recent = schema_id;
      return /* continue = */ false;
    }
    if (recent.empty())
      recent = schema_id;
    return /* continue = */ true;
  });
  return recent;
}

Schema* Switcher::CreateSchema() {
  const auto id = InitialSchemaId(schema_->config(), user_config_.get());
  return id.empty() ? nullptr : new Schema(id);
}

void Switcher::SelectNextSchema() {
  if (translators_.empty())
    return;
  auto xlator = translators_[0];  // schema_list_translator
  if (!xlator)
    return;
  Menu menu;
  menu.AddTranslation(xlator->Query("", Segment()));
  if (menu.Prepare(2) < 2)
    return;
  auto command = As<SwitcherCommand>(menu.GetCandidateAt(1));
  if (!command)
    return;
  command->Apply(this);
}

void Switcher::OnSelect(Context* ctx) {
  LOG(INFO) << "a switcher option is selected.";
  if (auto command = As<SwitcherCommand>(ctx->GetSelectedCandidate())) {
    command->Apply(this);
  }
}

void Switcher::RefreshMenu() {
  if (settings_menu_) {
    RefreshSettingsMenu(setting_group_);
    return;
  }
  Composition& comp = context_->composition();
  if (comp.empty()) {
    // no longer need this to make context_->IsComposing() == true
    // context_->set_input(" ");
    Segment seg(0, 0);  // empty range
    seg.prompt = caption_;
    comp.AddSegment(seg);
  }
  auto menu = New<Menu>();
  comp.back().menu = menu;
  for (auto& translator : translators_) {
    if (auto t = translator->Query("", comp.back())) {
      menu->AddTranslation(t);
    }
  }
}

void Switcher::Activate() {
  LOG(INFO) << "switcher is activated.";
  settings_menu_ = false;
  setting_group_.clear();
  context_->set_option("_fold_options", fold_options_);
  RefreshMenu();
  engine_->set_active_engine(this);
  active_ = true;
}

void Switcher::Deactivate() {
  settings_menu_ = false;
  setting_group_.clear();
  active_ = false;
  engine_->set_active_engine();
  context_->Clear();
}

void Switcher::DeactivateAndApply(function<void()> apply) {
  settings_menu_ = false;
  setting_group_.clear();
  active_ = false;
  engine_->set_active_engine();
  apply();
  context_->Clear();
}

void Switcher::LoadSettings() {
  Config* config = schema_->config();
  if (!config)
    return;
  if (!config->GetString("switcher/caption", &caption_) || caption_.empty()) {
    caption_ = ":-)";
  }
  if (auto hotkeys = config->GetList("switcher/hotkeys")) {
    hotkeys_.clear();
    for (size_t i = 0; i < hotkeys->size(); ++i) {
      auto value = hotkeys->GetValueAt(i);
      if (!value)
        continue;
      hotkeys_.push_back(KeyEvent(value->str()));
    }
  }
  config->GetBool("switcher/fold_options", &fold_options_);
  config->GetBool("switcher/fix_schema_list_order", &fix_schema_list_order_);
}

void Switcher::InitializeComponents() {
  processors_.clear();
  translators_.clear();
  if (auto c = Processor::Require("key_binder")) {
    an<Processor> p(c->Create(Ticket(this)));
    processors_.push_back(p);
  } else {
    LOG(WARNING) << "key_binder not available.";
  }
  if (auto c = Processor::Require("selector")) {
    an<Processor> p(c->Create(Ticket(this)));
    processors_.push_back(p);
  } else {
    LOG(WARNING) << "selector not available.";
  }
  DLOG(INFO) << "num processors: " << processors_.size();
  if (auto c = Translator::Require("schema_list_translator")) {
    an<Translator> t(c->Create(Ticket(this)));
    translators_.push_back(t);
  } else {
    LOG(WARNING) << "schema_list_translator not available.";
  }
  if (auto c = Translator::Require("switch_translator")) {
    an<Translator> t(c->Create(Ticket(this)));
    translators_.push_back(t);
  } else {
    LOG(WARNING) << "switch_translator not available.";
  }
  DLOG(INFO) << "num translators: " << translators_.size();
}

}  // namespace rime
