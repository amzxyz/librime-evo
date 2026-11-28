//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-04-24 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <cctype>
#include <chrono>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/config/config_types.h>
#include <rime/engine.h>
#include <rime/config/resource_config.h>
#include <rime/config/runtime_settings.h>
#include <rime/dict/auxiliary_db.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/mapped_file.h>
#include <rime/dict/rewrite_store.h>
#include <rime/gear/grammar.h>
#include <rime/filter.h>
#include <rime/formatter.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/processor.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/segmentor.h>
#include <rime/switcher.h>
#include <rime/switches.h>
#include <rime/ticket.h>
#include <rime/translation.h>
#include <rime/translator.h>

namespace rime {

struct Condition {
  enum Type { kOption, kAllOf, kAnyOf, kNoneOf } type = kOption;
  string option_name;
  bool expected_value = true;
  vector<Condition> children;
};

template <typename T>
struct ComponentEntry {
  an<T> component;
  string prescription;
  Condition condition;
  bool has_condition = false;
  bool enabled = true;
};

inline bool EvaluateCondition(const Condition& condition, Context* ctx) {
  if (!ctx)
    return false;
  switch (condition.type) {
    case Condition::kOption:
      return ctx->get_option(condition.option_name) == condition.expected_value;
    case Condition::kAllOf:
      for (const auto& child : condition.children) {
        if (!EvaluateCondition(child, ctx))
          return false;
      }
      return true;
    case Condition::kAnyOf:
      for (const auto& child : condition.children) {
        if (EvaluateCondition(child, ctx))
          return true;
      }
      return false;
    case Condition::kNoneOf:
      for (const auto& child : condition.children) {
        if (EvaluateCondition(child, ctx))
          return false;
      }
      return true;
  }
  return false;
}

template <typename T>
inline void UpdateComponentEnabled(ComponentEntry<T>& entry, Context* ctx) {
  if (!entry.has_condition) {
    entry.enabled = true;
    return;
  }
  entry.enabled = EvaluateCondition(entry.condition, ctx);
}

template <typename T>
inline void UpdateComponentConditions(vector<ComponentEntry<T>>& components,
                                      Context* ctx) {
  for (auto& entry : components) {
    UpdateComponentEnabled(entry, ctx);
  }
}

class ConcreteEngine : public Engine {
 public:
  ConcreteEngine();
  virtual ~ConcreteEngine();
  virtual bool ProcessKey(const KeyEvent& key_event) {
    return ProcessKey(key_event, /* synthetic= */ false);
  }
  virtual bool ProcessSyntheticKey(const KeyEvent& key_event) {
    return ProcessKey(key_event, /* synthetic= */ true);
  }
  virtual void ApplySchema(Schema* schema);
  virtual void CommitText(string text);
  virtual void Compose(Context* ctx);
  void GetCodeChoices(vector<CodeChoice>* result) override;
  bool SelectConfiguredSwitch(const string& option, bool value);
  bool SelectConfiguredSetting(const string& group, vector<string> values);

 protected:
  bool ProcessKey(const KeyEvent& key_event, bool synthetic);
  void InitializeComponents();
  void InitializeOptions();
  void RefreshOptionComponents(Context* ctx);
  void NotifyOption(Context* ctx, const string& option);
  void CalculateSegmentation(Segmentation* segments);
  void TranslateSegments(Segmentation* segments);
  void FormatText(string* text);
  void OnCommit(Context* ctx);
  void OnSelect(Context* ctx);
  void OnContextUpdate(Context* ctx);
  void OnOptionUpdate(Context* ctx, const string& option);
  void OnPropertyUpdate(Context* ctx, const string& property);
  void InitializeRuntimeSettings();

  vector<ComponentEntry<Processor>> processors_;
  vector<ComponentEntry<Segmentor>> segmentors_;
  vector<ComponentEntry<Translator>> translators_;
  vector<ComponentEntry<Filter>> filters_;
  vector<of<Formatter>> formatters_;
  vector<of<Processor>> post_processors_;
  an<Switcher> switcher_;
  bool applying_switch_ = false;
  set<string> schema_switches_;
};

// implementations

Engine* Engine::Create() {
  return new ConcreteEngine;
}

bool Engine::SelectSwitch(const string& option, bool value) {
  auto* engine = dynamic_cast<ConcreteEngine*>(this);
  return engine && engine->SelectConfiguredSwitch(option, value);
}

bool Engine::SelectSetting(const string& group, vector<string> values) {
  auto* engine = dynamic_cast<ConcreteEngine*>(this);
  return engine && engine->SelectConfiguredSetting(group, std::move(values));
}

Engine::Engine() : schema_(new Schema), context_(new Context) {}

Engine::~Engine() {
  context_.reset();
  schema_.reset();
}

ConcreteEngine::ConcreteEngine() {
  LOG(INFO) << "starting engine.";
  // receive context notifications
  context_->commit_notifier().connect([this](Context* ctx) { OnCommit(ctx); });
  context_->select_notifier().connect([this](Context* ctx) { OnSelect(ctx); });
  context_->update_notifier().connect(
      [this](Context* ctx) { OnContextUpdate(ctx); });
  context_->option_update_notifier().connect(
      [this](Context* ctx, const string& option) {
        OnOptionUpdate(ctx, option);
      });
  context_->property_update_notifier().connect(
      [this](Context* ctx, const string& property) {
        OnPropertyUpdate(ctx, property);
      });

  switcher_ = New<Switcher>(this);
  InitializeComponents();
}

ConcreteEngine::~ConcreteEngine() {
  LOG(INFO) << "engine disposed.";
}

bool ConcreteEngine::ProcessKey(const KeyEvent& key_event, bool synthetic) {
  DLOG(INFO) << "process key: " << key_event << ", synthetic: " << synthetic;
  ProcessResult ret = kNoop;
  for (auto& entry : processors_) {
    if (!entry.enabled)
      continue;
    ret = entry.component->ProcessKeyEvent(key_event);
    if (ret == kRejected)
      break;
    if (ret == kAccepted)
      return true;
  }
  // record unhandled keys, eg. spaces, numbers, bksp's.
  // skip internal synthetic key events which won't reach the client app.
  if (!synthetic) {
    context_->commit_history().Push(key_event);
  }
  // post-processing
  for (auto& processor : post_processors_) {
    ret = processor->ProcessKeyEvent(key_event);
    if (ret == kRejected)
      break;
    if (ret == kAccepted)
      return true;
  }
  // notify interested parties
  context_->unhandled_key_notifier()(context_.get(), key_event);
  return false;
}

void ConcreteEngine::OnContextUpdate(Context* ctx) {
  if (!ctx)
    return;
  Compose(ctx);
}

void ConcreteEngine::RefreshOptionComponents(Context* ctx) {
  UpdateComponentConditions(processors_, ctx);
  UpdateComponentConditions(segmentors_, ctx);
  UpdateComponentConditions(translators_, ctx);
  UpdateComponentConditions(filters_, ctx);
  if (ctx->IsComposing())
    ctx->RefreshNonConfirmedComposition();
}

void ConcreteEngine::NotifyOption(Context* ctx, const string& option) {
  const bool on = ctx->get_option(option);
  message_sink_("option", on ? option : "!" + option);
}

void ConcreteEngine::OnOptionUpdate(Context* ctx, const string& option) {
  if (!ctx || applying_switch_)
    return;
  LOG(INFO) << "updated option: " << option;
  RefreshOptionComponents(ctx);
  NotifyOption(ctx, option);
}

void ConcreteEngine::InitializeRuntimeSettings() {
  context_->ClearSettings();
  map<string, SettingGroupConfig> groups;
  if (!SettingGroupConfig::LoadGroups(schema_->config(), &groups))
    return;
  context_->SetSettingGroups(std::move(groups));
  auto* user = switcher_ ? switcher_->user_config() : nullptr;
  for (const auto& [group, setting] : context_->setting_groups()) {
    context_->SetSettingValues(
        group, setting.RestoreValues(user, schema_->schema_id(), group));
  }
}

bool ConcreteEngine::SelectConfiguredSetting(const string& group,
                                             vector<string> values) {
  const auto& groups = context_->setting_groups();
  auto found = groups.find(group);
  if (found == groups.end() || !found->second.Normalize(&values))
    return false;
  if (found->second.fixed)
    return false;
  const bool changed = context_->setting_values(group) != values;
  vector<size_t> reload;
  if (changed) {
    try {
      for (const auto& entry : translators_) {
        if (!entry.component->PrepareSetting(group, values))
          return false;
      }
    } catch (const std::exception& ex) {
      LOG(ERROR) << "cannot prepare setting '" << group << "': " << ex.what();
      return false;
    }
    for (size_t i = 0; i < translators_.size(); ++i) {
      const auto& entry = translators_[i];
      Ticket ticket(this, "translator", entry.prescription);
      if (ticket.klass != "script_translator" &&
          ticket.klass != "r10n_translator" &&
          ticket.klass != "table_translator")
        continue;
      string value, dependency;
      if (schema_->config()->GetString(ticket.name_space + "/user_dict_mode",
                                       &value) &&
          ParseSettingReference(value, &dependency) && dependency == group)
        reload.push_back(i);
    }
    for (size_t index : reload)
      if (!translators_[index].component->PrepareForReload())
        return false;
  }
  const auto previous = context_->setting_values(group);
  vector<pair<size_t, an<Translator>>> replacements;
  if (!reload.empty()) {
    context_->SetSettingValues(group, values);
    for (size_t index : reload) {
      const auto& entry = translators_[index];
      Ticket ticket(this, "translator", entry.prescription);
      auto* factory = Translator::Require(ticket.klass);
      an<Translator> next(factory ? factory->Create(ticket) : nullptr);
      if (!next) {
        context_->SetSettingValues(group, previous);
        return false;
      }
      replacements.emplace_back(index, std::move(next));
    }
  }
  auto* user = switcher_ ? switcher_->user_config() : nullptr;
  if (!found->second.SaveValues(user, schema_->schema_id(), group, values)) {
    if (!reload.empty())
      context_->SetSettingValues(group, previous);
    return false;
  }
  if (changed) {
    // Release all old candidate menus before replacing translators. Keep the
    // raw input and caret so the new translators can compose it again.
    const bool recompose = !reload.empty() && context_->IsComposing();
    if (recompose)
      context_->composition().clear();
    context_->SetSettingValues(group, std::move(values));
    for (auto& replacement : replacements)
      translators_[replacement.first].component = std::move(replacement.second);
    RefreshOptionComponents(context_.get());
    if (recompose)
      context_->update_notifier()(context_.get());
    message_sink_("setting", group);
  }
  return true;
}

void ConcreteEngine::OnPropertyUpdate(Context* ctx, const string& property) {
  if (!ctx)
    return;
  LOG(INFO) << "updated property: " << property;
  // notification
  string value = ctx->get_property(property);
  string msg(property + "=" + value);
  message_sink_("property", msg);
}

void ConcreteEngine::Compose(Context* ctx) {
  if (!ctx)
    return;
  Composition& comp = ctx->composition();
  const string active_input = ctx->input().substr(0, ctx->caret_pos());
  DLOG(INFO) << "active input: " << active_input;
  comp.Reset(active_input);
  if (ctx->caret_pos() < ctx->input().length() &&
      ctx->caret_pos() == comp.GetConfirmedPosition()) {
    // translate one segment past caret pos.
    comp.Reset(ctx->input());
  }
  CalculateSegmentation(&comp);
  TranslateSegments(&comp);
  DLOG(INFO) << "composition: [" << comp.GetDebugText() << "]";
}

void ConcreteEngine::GetCodeChoices(vector<CodeChoice>* result) {
  if (!result)
    return;
  result->clear();
  if (context_->get_option("ascii_mode") || active_engine() != this)
    return;
  const auto& input = context_->input();
  const size_t caret = context_->caret_pos();
  for (const auto& segment : context_->composition()) {
    if (segment.status >= Segment::kSelected || segment.start >= caret ||
        segment.end > caret || segment.auxiliary_suffix)
      continue;
    const auto code = input.substr(segment.start, segment.end - segment.start);
    for (auto& entry : translators_) {
      if (entry.enabled)
        entry.component->GetCodeChoices(code, segment, result);
    }
    if (!result->empty())
      break;
  }
  std::sort(result->begin(), result->end(),
            [](const CodeChoice& a, const CodeChoice& b) {
              if (a.start != b.start)
                return a.start < b.start;
              if (a.end != b.end)
                return a.end > b.end;
              if (a.label != b.label)
                return a.label < b.label;
              return a.replacement < b.replacement;
            });
  if (result->empty())
    return;
  const size_t first = result->front().start;
  result->erase(std::remove_if(result->begin(), result->end(),
                               [first](const CodeChoice& choice) {
                                 return choice.start != first;
                               }),
                result->end());
  result->erase(std::unique(result->begin(), result->end(),
                            [](const CodeChoice& a, const CodeChoice& b) {
                              return a.start == b.start && a.end == b.end &&
                                     a.label == b.label &&
                                     a.replacement == b.replacement;
                            }),
                result->end());
}

void ConcreteEngine::CalculateSegmentation(Segmentation* segments) {
  DLOG(INFO) << "CalculateSegmentation, segments: " << segments->size()
             << ", finished? " << segments->HasFinishedSegmentation();
  while (!segments->HasFinishedSegmentation()) {
    size_t start_pos = segments->GetCurrentStartPosition();
    size_t end_pos = segments->GetCurrentEndPosition();
    DLOG(INFO) << "start pos: " << start_pos;
    DLOG(INFO) << "end pos: " << end_pos;
    // recognize a segment by calling the segmentors in turn
    for (auto& entry : segmentors_) {
      if (!entry.enabled)
        continue;
      if (!entry.component->Proceed(segments))
        break;
    }
    DLOG(INFO) << "segmentation: " << *segments;
    // no advancement
    if (start_pos == segments->GetCurrentEndPosition())
      break;
    // only one segment is allowed past caret pos, which is the segment
    // immediately after the caret.
    if (start_pos >= context_->caret_pos())
      break;
    // move onto the next segment...
    if (!segments->HasFinishedSegmentation())
      segments->Forward();
  }
  // start an empty segment only at the end of a confirmed composition.
  if (!segments->empty() && !segments->back().HasTag("placeholder"))
    segments->Trim();
  if (!segments->empty() && segments->back().status >= Segment::kSelected)
    segments->Forward();
}

void ConcreteEngine::TranslateSegments(Segmentation* segments) {
  DLOG(INFO) << "TranslateSegments: " << *segments;
  for (Segment& segment : *segments) {
    DLOG(INFO) << "segment [" << segment.start << ", " << segment.end
               << "), status: " << segment.status;
    if (segment.status >= Segment::kGuess)
      continue;
    size_t len = segment.end - segment.start;
    string input = segments->input().substr(segment.start, len);
    DLOG(INFO) << "translating segment: [" << input << "]";
    auto menu = New<Menu>();
    for (auto& entry : translators_) {
      if (!entry.enabled || (segment.auxiliary_suffix &&
                             !entry.component->SupportsAuxiliarySuffix()))
        continue;
      auto translation = entry.component->Query(input, segment);
      if (!translation)
        continue;
      if (translation->exhausted()) {
        DLOG(INFO) << entry.component->name_space()
                   << " made a futile translation.";
        continue;
      }
      menu->AddTranslation(translation);
    }
    for (auto& entry : filters_) {
      if (entry.enabled && entry.component->AppliesToSegment(&segment)) {
        menu->AddFilter(entry.component.get());
      }
    }
    segment.status = Segment::kGuess;
    segment.menu = menu;
    segment.selected_index = 0;
  }
}

void ConcreteEngine::FormatText(string* text) {
  if (formatters_.empty())
    return;
  DLOG(INFO) << "applying formatters.";
  for (auto& formatter : formatters_) {
    formatter->Format(text);
  }
}

void ConcreteEngine::CommitText(string text) {
  context_->commit_history().Push(CommitRecord{"raw", text});
  FormatText(&text);
  DLOG(INFO) << "committing text: " << text;
  sink_(text);
}

void ConcreteEngine::OnCommit(Context* ctx) {
  context_->commit_history().Push(ctx->composition(), ctx->input());
  string text = ctx->GetCommitText();
  FormatText(&text);
  DLOG(INFO) << "committing composition: " << text;
  sink_(text);
}

void ConcreteEngine::OnSelect(Context* ctx) {
  Segment& seg(ctx->composition().back());
  seg.Close();
  if (seg.end == ctx->input().length()) {
    // composition has finished
    seg.status = Segment::kConfirmed;
    // strategy one: commit directly;
    // strategy two: continue composing with another empty segment.
    if (ctx->get_option("_auto_commit"))
      ctx->Commit();
    else
      ctx->composition().Forward();
  } else {
    bool reached_caret_pos = (seg.end >= ctx->caret_pos());
    ctx->composition().Forward();
    if (reached_caret_pos) {
      // finished converting current segment
      // move caret to the end of input
      ctx->set_caret_pos(ctx->input().length());
    } else {
      Compose(ctx);
    }
  }
}

void ConcreteEngine::ApplySchema(Schema* schema) {
  if (!schema)
    return;
  if (switcher_ && switcher_->active())
    switcher_->Deactivate();
  // 支持 this->schema() 原位重新加載
  if (schema != schema_.get()) {
    schema_.reset(schema);
  }
  context_->Clear();
  context_->ClearTransientOptions();
  InitializeComponents();
  switcher_->SetActiveSchema(schema_->schema_id());
  message_sink_("schema", schema_->schema_id() + "/" + schema_->schema_name());
}

// Helper template function to create components
inline bool HasOnlyKeys(ConfigMap* map, const set<string>& allowed_keys) {
  if (!map)
    return false;
  for (auto it = map->begin(); it != map->end(); ++it) {
    if (allowed_keys.find(it->first) == allowed_keys.end())
      return false;
  }
  return true;
}

inline bool HasExactKeys(ConfigMap* map, const set<string>& expected_keys) {
  if (!map)
    return false;
  size_t key_count = 0;
  for (auto it = map->begin(); it != map->end(); ++it)
    ++key_count;
  if (key_count != expected_keys.size())
    return false;
  return HasOnlyKeys(map, expected_keys);
}

constexpr int kMaxConditionDepth = 32;

inline bool ParseCondition(const an<ConfigItem>& item,
                           Condition* condition,
                           int depth = 0) {
  if (!condition || depth > kMaxConditionDepth)
    return false;

  if (auto value = As<ConfigValue>(item)) {
    string option = value->str();
    if (option.empty())
      return false;
    condition->type = Condition::kOption;
    condition->expected_value = true;
    if (option[0] == '!') {
      condition->expected_value = false;
      option.erase(0, 1);
    }
    if (option.empty())
      return false;
    condition->option_name = option;
    condition->children.clear();
    return true;
  }

  auto map = As<ConfigMap>(item);
  if (!map)
    return false;

  // Explicit option conditions are useful when a boolean value is needed.
  if (map->HasKey("option")) {
    if (!HasExactKeys(map.get(), {"option", "value"}))
      return false;
    auto option = map->GetValue("option");
    auto value = map->GetValue("value");
    if (!option || option->str().empty() || !value ||
        !value->GetBool(&condition->expected_value))
      return false;
    condition->type = Condition::kOption;
    condition->option_name = option->str();
    condition->children.clear();
    return true;
  }

  string operation;
  Condition::Type type = Condition::kOption;
  if (map->HasKey("all_of")) {
    operation = "all_of";
    type = Condition::kAllOf;
  } else if (map->HasKey("any_of")) {
    operation = "any_of";
    type = Condition::kAnyOf;
  } else if (map->HasKey("none_of")) {
    operation = "none_of";
    type = Condition::kNoneOf;
  } else {
    return false;
  }
  if (!HasExactKeys(map.get(), {operation}))
    return false;

  auto list = As<ConfigList>(map->Get(operation));
  if (!list || list->empty())
    return false;
  condition->type = type;
  condition->option_name.clear();
  condition->children.clear();
  for (size_t i = 0; i < list->size(); ++i) {
    Condition child;
    if (!ParseCondition(list->GetAt(i), &child, depth + 1))
      return false;
    condition->children.push_back(child);
  }
  return true;
}

template <typename T>
inline bool ParseComponentSpec(const an<ConfigItem>& item,
                               string* prescription,
                               ComponentEntry<T>* entry) {
  if (auto value = As<ConfigValue>(item)) {
    *prescription = value->str();
    return true;
  }

  auto map = As<ConfigMap>(item);
  if (!map || !HasOnlyKeys(map.get(), {"name", "when"}))
    return false;

  auto name = map->GetValue("name");
  if (!name || name->str().empty())
    return false;
  *prescription = name->str();

  auto when_node = map->Get("when");
  if (!when_node)
    return true;
  if (!ParseCondition(when_node, &entry->condition))
    return false;
  entry->has_condition = true;
  return true;
}

void Engine::PrepareSchemaResources(Schema* schema, vector<an<void>>* pins) {
  Config* config = schema ? schema->config() : nullptr;
  if (!config)
    return;
  auto* user_component = Config::Require("user_config");
  the<Config> user(user_component ? user_component->Create("user") : nullptr);
  set<string> dictionaries, auxiliary, rewrites;
  bool grammar = false;
  // Isolate optional resource failures. Collect only mappings associated
  // with a successfully retained object; keep actual load order.
  auto prepare = [&](const string& label, auto loader) {
    try {
      vector<path> files;
      an<void> pin;
      {
        ReadOnlyResourceCollector collector(&files);
        pin = loader();
      }
      if (!pin)
        return;
      for (const auto& file : files)
        ReadOnlyResourceCollector::Record(file);
      pins->push_back(std::move(pin));
    } catch (const std::exception& ex) {
      LOG(WARNING) << "prewarm '" << label << "' failed: " << ex.what();
    } catch (...) {
      LOG(WARNING) << "prewarm '" << label << "' failed.";
    }
  };
  auto add_rewrite = [&](const string& name_space) {
    string data;
    if (config->GetString(name_space + "/data", &data) &&
        ResolveResourceReference(config, "rewrite", &data) && !data.empty())
      rewrites.insert(data);
  };
  if (auto list = config->GetList("engine/translators")) {
    for (const auto& item : *list) {
      string prescription;
      ComponentEntry<Translator> entry;
      if (!ParseComponentSpec(item, &prescription, &entry))
        continue;
      Ticket ticket(nullptr, "translator", prescription);
      ticket.schema = schema;
      // Reverse lookup is intentionally lazy, and plugin translators may have
      // side effects. Never construct a translator or open a UserDB here.
      const bool scripted = ticket.klass == "script_translator" ||
                            ticket.klass == "r10n_translator";
      if (!scripted && ticket.klass != "table_translator")
        continue;
      const auto& ns = ticket.name_space;
      if (dictionaries.insert(ns).second) {
        string name;
        auto* factory = Dictionary::Require("dictionary");
        if (factory && config->GetString(ns + "/dictionary", &name) &&
            ResolveResourceReference(config, "dictionary", &name) &&
            !name.empty()) {
          prepare(name, [&]() -> an<void> {
            an<Dictionary> dict(factory->Create(ticket));
            return dict && dict->Load() ? dict : nullptr;
          });
        }
      }
      string charset;
      if (config->GetString(ns + "/charset", &charset))
        add_rewrite(charset);
      if (scripted) {
        grammar = true;
        string aux;
        if (config->GetString(ns + "/aux", &aux)) {
          string group;
          ParseSettingReference(aux, &group);
          if (!group.empty()) {
            SettingGroupConfig types;
            if (types.Load(config, group)) {
              const auto selected =
                  types.RestoreValues(user.get(), schema->schema_id(), group);
              auxiliary.insert(selected.begin(), selected.end());
            }
          } else if (ResolveResourceReference(config, "auxiliary", &aux) &&
                     !aux.empty()) {
            auxiliary.insert(aux);
          }
        }
      } else {
        bool sentence = true, completion = false, contextual = false;
        config->GetBool(ns + "/enable_sentence", &sentence);
        config->GetBool(ns + "/sentence_over_completion", &completion);
        config->GetBool(ns + "/contextual_suggestions", &contextual);
        grammar = grammar || sentence || completion || contextual;
      }
    }
  }
  if (auto list = config->GetList("engine/filters")) {
    for (const auto& item : *list) {
      string prescription;
      ComponentEntry<Filter> entry;
      if (!ParseComponentSpec(item, &prescription, &entry))
        continue;
      Ticket ticket(nullptr, "filter", prescription);
      if (ticket.klass == "rewriter") {
        const string ns =
            ticket.name_space == "filter" ? "rewriter" : ticket.name_space;
        add_rewrite(ns);
      }
    }
  }
  string language;
  if (grammar && config->GetString("grammar/language", &language) &&
      !language.empty()) {
    if (auto* factory = Grammar::Require("grammar")) {
      // The grammar plugin itself resolves its model (.dlm, .gram, etc.).
      prepare(language, [&]() -> an<void> {
        return an<Grammar>(factory->Create(config));
      });
    }
  }
  for (const auto& name : auxiliary) {
    prepare(name, [&]() -> an<void> { return AuxiliaryDb::OpenShared(name); });
  }
  for (const auto& name : rewrites) {
    prepare(name, [&]() -> an<void> {
      auto resource = RewriteResource::Require(name);
      return resource && resource->Acquire() ? resource : nullptr;
    });
  }
}

template <typename T>
inline void CreateComponentsFromList(
    Engine* engine,
    Config* config,
    const string& config_key,
    const string& component_type,
    vector<ComponentEntry<T>>& target_collection) {
  if (auto component_list = config->GetList(config_key)) {
    size_t n = component_list->size();
    for (size_t i = 0; i < n; ++i) {
      string prescription;
      ComponentEntry<T> entry;
      if (!ParseComponentSpec<T>(component_list->GetAt(i), &prescription,
                                 &entry)) {
        LOG(ERROR) << "invalid " << component_type << " prescription at index "
                   << i << " in '" << config_key << "'";
        continue;
      }
      Ticket ticket{engine, component_type, prescription};
      auto c = T::Require(ticket.klass);
      if (!c) {
        LOG(ERROR) << "error creating " << component_type << ": '"
                   << ticket.klass << "'";
        continue;
      }
      const auto create_start = std::chrono::steady_clock::now();
      auto component = c->Create(ticket);
      const auto create_cost_us =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - create_start)
              .count();
      const auto create_cost_ms = create_cost_us / 1000.0;
      LOG(INFO) << "[ComponentTiming] type=" << component_type
                << " prescription='" << prescription << "'"
                << " class='" << ticket.klass << "'"
                << " namespace='" << ticket.name_space << "'"
                << " cost_us=" << create_cost_us
                << " cost_ms=" << create_cost_ms
                << " success=" << (component != nullptr);
      if (!component) {
        LOG(ERROR) << "error creating " << component_type << " from ticket: '"
                   << ticket.klass << "'";
        continue;
      }
      an<T> instance(component);
      entry.component = instance;
      entry.prescription = prescription;
      UpdateComponentEnabled(entry, engine->context());
      target_collection.push_back(entry);
    }
  }
}

void ConcreteEngine::InitializeComponents() {
  processors_.clear();
  segmentors_.clear();
  translators_.clear();
  filters_.clear();
  formatters_.clear();
  post_processors_.clear();
  context_->ClearSchemaOptions(schema_switches_);
  schema_switches_.clear();

  if (switcher_) {
    ComponentEntry<Processor> entry;
    entry.component = switcher_;
    processors_.push_back(entry);
    if (schema_->schema_id() == ".default") {
      if (Schema* schema = switcher_->CreateSchema()) {
        schema_.reset(schema);
      }
    }
  }

  Config* config = schema_->config();
  if (!config)
    return;

  InitializeRuntimeSettings();
  InitializeOptions();

  // Create components using inline template function
  CreateComponentsFromList<Processor>(this, config, "engine/processors",
                                      "processor", processors_);
  CreateComponentsFromList<Segmentor>(this, config, "engine/segmentors",
                                      "segmentor", segmentors_);
  CreateComponentsFromList<Translator>(this, config, "engine/translators",
                                       "translator", translators_);
  CreateComponentsFromList<Filter>(this, config, "engine/filters", "filter",
                                   filters_);
  // create formatters
  auto c_formatter = Formatter::Require("shape_formatter");
  if (c_formatter) {
    an<Formatter> f(c_formatter->Create(Ticket(this)));
    if (f) {
      formatters_.push_back(f);
    } else {
      LOG(WARNING) << "failed to create shape_formatter.";
    }
  } else {
    LOG(WARNING) << "shape_formatter not available.";
  }
  // create post-processors
  auto c_processor = Processor::Require("shape_processor");
  if (c_processor) {
    an<Processor> p(c_processor->Create(Ticket(this)));
    if (p) {
      post_processors_.push_back(p);
    } else {
      LOG(WARNING) << "failed to create shape_processor.";
    }
  } else {
    LOG(WARNING) << "shape_processor not available.";
  }
}

namespace {

using SwitchOption = Switches::SwitchOption;

vector<SwitchOption> GetMembers(Switches& switches, const SwitchOption& first) {
  vector<SwitchOption> members;
  switches.FindOption([&](SwitchOption option) {
    if (option.switch_index == first.switch_index)
      members.push_back(std::move(option));
    return Switches::kContinue;
  });
  return members;
}

bool PublicSwitchName(const string& name) {
  return !name.empty() && name.front() != '_' && name.find('/') == string::npos;
}

string SwitchPath(const string& schema, const string& option) {
  return "var/schema_options/" + schema + "/" + option;
}

bool SaveSwitchValues(Config* user,
                      const string& schema,
                      const vector<SwitchOption>& members,
                      const vector<bool>& values) {
  if (!user)
    return false;
  bool changed = false;
  for (size_t i = 0; i < members.size(); ++i) {
    const string path = SwitchPath(schema, members[i].option_name);
    bool old = false;
    if (user->GetBool(path, &old) && old == values[i])
      continue;
    if (!user->SetBool(path, values[i]))
      LOG(ERROR) << "cannot save switch: " << path;
    else
      changed = true;
  }
  return changed;
}

}  // namespace

void ConcreteEngine::InitializeOptions() {
  Config* config = schema_->config();
  if (!config)
    return;
  auto* user = switcher_ ? switcher_->user_config() : nullptr;
  const auto& schema_id = schema_->schema_id();
  Switches switches(config);
  bool changed = false;
  vector<vector<SwitchOption>> groups;
  switches.FindOption([&](SwitchOption option) {
    if (groups.empty() ||
        groups.back().front().switch_index != option.switch_index)
      groups.emplace_back();
    groups.back().push_back(std::move(option));
    return Switches::kContinue;
  });
  for (const auto& members : groups) {
    const auto& first = members.front();
    bool public_names = true;
    for (const auto& member : members) {
      public_names &= PublicSwitchName(member.option_name);
      if (!member.option_name.empty())
        schema_switches_.insert(member.option_name);
    }
    const bool radio = first.type == Switches::kRadioGroup;
    const bool remember = first.reset_value < 0 && public_names &&
                          !schema_id.empty() &&
                          schema_id.find('/') == string::npos;
    vector<bool> values(members.size(), false);
    bool has_saved_value = false;
    if (remember && user) {
      for (size_t i = 0; i < members.size(); ++i) {
        bool saved = false;
        if (user->GetBool(SwitchPath(schema_id, members[i].option_name),
                          &saved)) {
          values[i] = saved;
          has_saved_value = true;
        }
      }
    }
    if (radio) {
      size_t selected = 0;
      if (first.reset_value >= 0) {
        if (static_cast<size_t>(first.reset_value) < members.size())
          selected = first.reset_value;
      } else {
        const auto found = std::find(values.begin(), values.end(), true);
        if (found != values.end())
          selected = found - values.begin();
      }
      for (size_t i = 0; i < members.size(); ++i)
        values[i] = i == selected;
    } else if (first.reset_value >= 0) {
      values[0] = first.reset_value != 0;
    }
    for (size_t i = 0; i < members.size(); ++i) {
      if (!members[i].option_name.empty())
        context_->set_option(members[i].option_name, values[i]);
    }
    // Normalize a previously saved radio group to exactly one selection.
    if (remember && radio && has_saved_value)
      changed |= SaveSwitchValues(user, schema_id, members, values);
  }
  if (changed && user && !user->Save())
    LOG(ERROR) << "cannot normalize saved switches";
}

bool ConcreteEngine::SelectConfiguredSwitch(const string& option, bool value) {
  if (!schema_->config() || !PublicSwitchName(option))
    return false;
  Switches switches(schema_->config());
  auto selected = switches.OptionByName(option);
  if (!selected.found())
    return false;
  const auto members = GetMembers(switches, selected);
  for (const auto& member : members) {
    if (!PublicSwitchName(member.option_name))
      return false;
  }
  const bool radio = selected.type == Switches::kRadioGroup;
  // A radio selection selects one member, never deselects the whole group.
  if (radio && !value)
    return true;
  vector<bool> values;
  vector<string> changed;
  applying_switch_ = true;
  for (const auto& member : members) {
    const bool selected_member = member.option_index == selected.option_index;
    const bool next = radio ? selected_member : value;
    values.push_back(next);
    if (context_->get_option(member.option_name) != next) {
      context_->set_option(member.option_name, next);
      changed.push_back(member.option_name);
    }
  }
  applying_switch_ = false;
  if (!changed.empty()) {
    RefreshOptionComponents(context_.get());
    for (const auto& name : changed)
      NotifyOption(context_.get(), name);
  }
  const auto& schema_id = schema_->schema_id();
  if (selected.reset_value < 0 && switcher_ && !schema_id.empty() &&
      schema_id.find('/') == string::npos) {
    auto* user = switcher_->user_config();
    if (SaveSwitchValues(user, schema_id, members, values) && !user->Save())
      LOG(ERROR) << "cannot save switches";
  }
  return true;
}

}  // namespace rime
