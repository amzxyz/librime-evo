#include <gtest/gtest.h>
#include <sstream>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/candidate.h>
#include <rime/config/resource_config.h>
#include <rime/config/runtime_settings.h>
#include <rime/dict/schema_resource_validation.h>
#include <rime/schema.h>
#include <rime_api.h>

namespace rime {
namespace {

Schema* MakeSwitchSchema(const string& id, bool reset = false) {
  auto config = new Config;
  const char* forced = R"(
switches:
  - {name: scoped_emoji, reset: 1}
  - {options: [scoped_a, scoped_b], reset: 1}
)";
  const char* remembered = R"(
switches:
  - {name: scoped_emoji}
  - {options: [scoped_a, scoped_b]}
)";
  std::istringstream yaml(reset ? forced : remembered);
  EXPECT_TRUE(config->LoadFromStream(yaml));
  return new Schema(id, config);
}

TEST(SchemaSwitchesTest, ResourceSymbolsAndFixedSettings) {
  string name;
  EXPECT_TRUE(ParseResourceSymbol("$pinyin", &name));
  EXPECT_EQ("pinyin", name);
  EXPECT_FALSE(ParseResourceSymbol("${input_types}", &name));
  EXPECT_TRUE(ParseSettingReference("${input_types}", &name));
  EXPECT_EQ("input_types", name);
  EXPECT_FALSE(ParseSettingReference("prefix_${input_types}", &name));
  Config config;
  std::istringstream yaml(R"(
runtime_settings:
  input_types:
    type: prism
    mode: single
    default: $pinyin
    fixed: $zrm
    values:
      - {value: $pinyin, label: Full}
      - {value: $zrm, label: Double}
)");
  ASSERT_TRUE(config.LoadFromStream(yaml));
  map<string, SettingGroupConfig> groups;
  ASSERT_TRUE(SettingGroupConfig::LoadGroups(&config, &groups));
  ASSERT_EQ(1u, groups.size());
  const auto& setting = groups.at("input_types");
  EXPECT_EQ("prism", setting.resource_type);
  const auto restored = setting.RestoreValues(nullptr, "test", "input_types");
  EXPECT_EQ(vector<string>{"zrm"}, restored);
  Config user;
  EXPECT_FALSE(setting.SaveValues(&user, "test", "input_types", {"pinyin"}));
}

TEST(SchemaSwitchesTest, RuntimeChoicesRequireValueKey) {
  Config config;
  std::istringstream yaml(R"(
runtime_settings:
  layout:
    type: prism
    mode: single
    values: [{prism: $pinyin}]
  auxiliary:
    type: auxiliary
    mode: single
    values: [{aux: $aux}]
  feature:
    mode: single
    values: [{id: off}]
)");
  ASSERT_TRUE(config.LoadFromStream(yaml));
  SettingGroupConfig setting;
  EXPECT_FALSE(setting.Load(&config, "layout"));
  EXPECT_FALSE(setting.Load(&config, "auxiliary"));
  EXPECT_FALSE(setting.Load(&config, "feature"));
  ASSERT_TRUE(
      config.SetString("runtime_settings/layout/values/@0/value", "$pinyin"));
  ASSERT_TRUE(
      config.SetString("runtime_settings/auxiliary/values/@0/value", "$aux"));
  ASSERT_TRUE(
      config.SetString("runtime_settings/feature/values/@0/value", "off"));
  ASSERT_TRUE(setting.Load(&config, "layout"));
  EXPECT_EQ((vector<string>{"pinyin"}), setting.defaults);
  ASSERT_TRUE(setting.Load(&config, "auxiliary"));
  EXPECT_EQ((vector<string>{"aux"}), setting.defaults);
  ASSERT_TRUE(setting.Load(&config, "feature"));
  EXPECT_EQ((vector<string>{"off"}), setting.defaults);
  ASSERT_TRUE(
      config.SetString("runtime_settings/layout/values/@0/value", "pinyin"));
  EXPECT_FALSE(setting.Load(&config, "layout"));
}

TEST(SchemaSwitchesTest, RuntimeSettingsPreserveChoiceOrder) {
  Config config;
  std::istringstream yaml(R"(
runtime_settings:
  z_group:
    mode: single
    values:
      - {value: z, label: Z}
      - {value: a, label: A}
  a_group:
    mode: multiple
    values:
      - {value: b}
      - {value: a}
)");
  ASSERT_TRUE(config.LoadFromStream(yaml));
  map<string, SettingGroupConfig> groups;
  ASSERT_TRUE(SettingGroupConfig::LoadGroups(&config, &groups));
  EXPECT_EQ((vector<string>{"z"}), groups.at("z_group").defaults);
  vector<string> selected{"a", "b"};
  ASSERT_TRUE(groups.at("a_group").Normalize(&selected));
  EXPECT_EQ((vector<string>{"b", "a"}), selected);
  ASSERT_TRUE(
      config.SetString("runtime_settings/z_group/values/@1/value", "z"));
  EXPECT_FALSE(SettingGroupConfig::LoadGroups(&config, &groups));
  EXPECT_TRUE(groups.empty());
}

TEST(SchemaSwitchesTest, FixedSettingPreservesSavedChoice) {
  Config config, user;
  std::istringstream yaml(R"(
runtime_settings:
  layout:
    mode: single
    default: full
    fixed: compact
    values:
      - {value: full, label: Full}
      - {value: compact, label: Compact}
)");
  ASSERT_TRUE(config.LoadFromStream(yaml));
  ASSERT_TRUE(user.SetString("var/type_groups/test/layout", "full"));
  SettingGroupConfig setting;
  ASSERT_TRUE(setting.Load(&config, "layout"));
  EXPECT_EQ((vector<string>{"compact"}),
            setting.RestoreValues(&user, "test", "layout"));
  EXPECT_FALSE(setting.SaveValues(&user, "test", "layout", {"full"}));
  ASSERT_TRUE(config.SetItem("runtime_settings/layout/fixed", nullptr));
  ASSERT_TRUE(setting.Load(&config, "layout"));
  EXPECT_EQ((vector<string>{"full"}),
            setting.RestoreValues(&user, "test", "layout"));
}

TEST(SchemaSwitchesTest, ResolveResourcesWithoutRewritingConfig) {
  Config config;
  std::istringstream yaml(R"(
build:
  $example:
    type: dictionary
    prism:
      $pinyin:
        - derive/^zh/z/
  $aux_code:
    type: auxiliary
    aux:
      $aux_example:
        - derive/a/b/
  $emoji:
    type: rewrite
    files:
      - data/emoji.txt
translator:
  dictionary: $example
  prism: $pinyin
  aux: $aux_example
emoji:
  data: $emoji
  option: emoji
)");
  ASSERT_TRUE(config.LoadFromStream(yaml));
  vector<BuildResource> resources;
  ASSERT_TRUE(ReadBuildResources(&config, &resources));
  EXPECT_EQ(3u, resources.size());
  string name = "$example";
  EXPECT_TRUE(ResolveResourceReference(&config, "dictionary", &name));
  EXPECT_EQ("example", name);
  name = "$pinyin";
  EXPECT_TRUE(ResolvePrismReference(&config, "example", &name));
  EXPECT_EQ("pinyin", name);
  name = "$aux_example";
  EXPECT_TRUE(ResolveResourceReference(&config, "auxiliary", &name));
  EXPECT_EQ("aux_example", name);
  name = "$emoji";
  EXPECT_TRUE(ResolveResourceReference(&config, "rewrite", &name));
  EXPECT_EQ("emoji", name);
  name = "$emoji";
  EXPECT_FALSE(ResolveResourceReference(&config, "dictionary", &name));
  name = "$missing";
  EXPECT_FALSE(ResolvePrismReference(&config, "example", &name));
  EXPECT_TRUE(config.GetList("build/$example/prism/$pinyin"));
  EXPECT_TRUE(config.GetList("build/$aux_code/aux/$aux_example"));
  EXPECT_TRUE(config.GetList("build/$emoji/files"));
  EXPECT_TRUE(config.GetString("translator/dictionary", &name));
  EXPECT_EQ("$example", name);
  EXPECT_TRUE(config.GetResource("translator/dictionary", &name));
  EXPECT_EQ("example", name);
  EXPECT_TRUE(config.GetResource("translator/aux", &name));
  EXPECT_EQ("aux_example", name);
  EXPECT_TRUE(config.GetResource("emoji/data", &name));
  EXPECT_EQ("emoji", name);
  EXPECT_TRUE(config.GetResource("translator/prism", &name));
  EXPECT_EQ("pinyin", name);
  ASSERT_TRUE(config.SetString("other/dictionary", "$emoji"));
  EXPECT_FALSE(config.GetResource("other/dictionary", &name));
  EXPECT_EQ("pinyin", name);
  ASSERT_TRUE(config.SetString("other/prism", "$pinyin"));
  EXPECT_FALSE(config.GetResource("other/prism", &name));
  ASSERT_TRUE(config.SetString("other/data", "$missing"));
  EXPECT_FALSE(config.GetResource("other/data", &name));
  ASSERT_TRUE(config.SetString("other/dictionary", "example"));
  EXPECT_TRUE(config.GetResource("other/prism", &name));
  EXPECT_EQ("pinyin", name);
  ASSERT_TRUE(config.SetItem("other", nullptr));
  EXPECT_FALSE(config.GetResource("missing", &name));
  ASSERT_TRUE(config.SetString("test_resource", "${input_types}"));
  EXPECT_FALSE(config.GetResource("test_resource", &name));
  ASSERT_TRUE(config.SetString("test_resource", "external"));
  EXPECT_TRUE(config.GetResource("test_resource", &name));
  EXPECT_EQ("external", name);
  EXPECT_TRUE(config.GetString("translator/dictionary", &name));
  EXPECT_EQ("$example", name);
  EXPECT_TRUE(ValidateSchemaResources(&config));
}

TEST(SchemaSwitchesTest, IsolatedSelectionAndTemporaryState) {
  the<Config> user(Config::Require("user_config")->Create("user"));
  const string a = "var/schema_options/switch_test_a";
  const string b = "var/schema_options/switch_test_b";
  user->SetItem(a, nullptr);
  user->SetItem(b, nullptr);
  user->SetItem("var/option/scoped_emoji", nullptr);
  user->SetItem("var/option/scoped_a", nullptr);
  user->SetItem("var/option/scoped_b", nullptr);
  the<Engine> engine(Engine::Create());
  engine->ApplySchema(MakeSwitchSchema("switch_test_a"));
  auto* ctx = engine->context();
  EXPECT_FALSE(ctx->get_option("scoped_emoji"));
  EXPECT_TRUE(ctx->get_option("scoped_a"));
  EXPECT_FALSE(engine->SelectSetting("scoped_emoji", {"true"}));
  EXPECT_FALSE(engine->SelectSetting("scoped_a", {"scoped_b"}));
  EXPECT_TRUE(engine->SelectSwitch("scoped_emoji", true));
  EXPECT_TRUE(engine->SelectSwitch("scoped_b", true));
  EXPECT_TRUE(engine->SelectSwitch("scoped_b", false));
  EXPECT_TRUE(ctx->get_option("scoped_b"));
  EXPECT_FALSE(ctx->get_option("scoped_a"));
  ctx->set_option("scoped_emoji", false);
  engine->ApplySchema(MakeSwitchSchema("switch_test_b"));
  EXPECT_FALSE(ctx->get_option("scoped_emoji"));
  EXPECT_TRUE(ctx->get_option("scoped_a"));
  EXPECT_TRUE(engine->SelectSwitch("scoped_emoji", false));
  engine->ApplySchema(MakeSwitchSchema("switch_test_a"));
  EXPECT_TRUE(ctx->get_option("scoped_emoji"));
  EXPECT_TRUE(ctx->get_option("scoped_b"));
  engine.reset(Engine::Create());
  engine->ApplySchema(MakeSwitchSchema("switch_test_a"));
  EXPECT_TRUE(engine->context()->get_option("scoped_emoji"));
  EXPECT_TRUE(engine->context()->get_option("scoped_b"));
  EXPECT_FALSE(engine->SelectSwitch("_no_learning", true));
  EXPECT_FALSE(engine->SelectSwitch("undeclared", true));
  user->SetItem(a, nullptr);
  user->SetItem(b, nullptr);
  EXPECT_TRUE(user->Save());
}

TEST(SchemaSwitchesTest, ResetWithoutLegacyMigration) {
  the<Config> user(Config::Require("user_config")->Create("user"));
  const string path = "var/schema_options/switch_test_reset";
  user->SetItem(path, nullptr);
  user->SetBool("var/option/scoped_emoji", true);
  user->SetBool("var/option/scoped_a", false);
  user->SetBool("var/option/scoped_b", true);
  the<Engine> engine(Engine::Create());
  engine->ApplySchema(MakeSwitchSchema("switch_test_reset"));
  auto* ctx = engine->context();
  // Legacy global values are ignored: toggles start false, radio starts first.
  EXPECT_FALSE(ctx->get_option("scoped_emoji"));
  EXPECT_TRUE(ctx->get_option("scoped_a"));
  EXPECT_FALSE(ctx->get_option("scoped_b"));
  bool saved = false;
  EXPECT_FALSE(user->GetBool(path + "/scoped_emoji", &saved));
  EXPECT_FALSE(user->GetBool(path + "/scoped_a", &saved));
  EXPECT_FALSE(user->GetBool(path + "/scoped_b", &saved));

  EXPECT_TRUE(engine->SelectSwitch("scoped_emoji", true));
  EXPECT_TRUE(engine->SelectSwitch("scoped_b", true));
  engine->ApplySchema(MakeSwitchSchema("switch_test_reset", true));
  EXPECT_TRUE(ctx->get_option("scoped_emoji"));
  EXPECT_TRUE(ctx->get_option("scoped_b"));
  EXPECT_TRUE(engine->SelectSwitch("scoped_emoji", false));
  EXPECT_TRUE(engine->SelectSwitch("scoped_a", true));
  engine->ApplySchema(MakeSwitchSchema("switch_test_reset", true));
  EXPECT_TRUE(ctx->get_option("scoped_emoji"));
  EXPECT_TRUE(ctx->get_option("scoped_b"));
  engine->ApplySchema(MakeSwitchSchema("switch_test_reset"));
  EXPECT_TRUE(ctx->get_option("scoped_emoji"));
  EXPECT_TRUE(ctx->get_option("scoped_b"));

  user->SetItem(path, nullptr);
  user->SetItem("var/option/scoped_emoji", nullptr);
  user->SetItem("var/option/scoped_a", nullptr);
  user->SetItem("var/option/scoped_b", nullptr);
  EXPECT_TRUE(user->Save());
}

TEST(SchemaSwitchesTest, NativeRuntimeSettingsMenu) {
  the<Config> defaults(Config::Require("config")->Create("default"));
  auto old_hotkeys = defaults->GetItem("switcher/hotkeys");
  auto hotkeys = New<ConfigList>();
  hotkeys->Append(New<ConfigValue>("Control+grave"));
  ASSERT_TRUE(defaults->SetItem("switcher/hotkeys", hotkeys));
  the<Config> user(Config::Require("user_config")->Create("user"));
  const string path = "var/type_groups/switch_test_menu";
  user->SetItem(path, nullptr);
  the<Engine> engine(Engine::Create());
  defaults->SetItem("switcher/hotkeys", old_hotkeys);
  auto config = new Config;
  std::istringstream yaml(R"(
runtime_settings:
  a_input:
    label: Input
    mode: single
    default: full
    values: [{value: full, label: Full}, {value: compact, label: Compact}]
  b_fuzzy:
    label: Fuzzy
    mode: multiple
    default: []
    values: [{value: a, label: A}, {value: b, label: B}]
  c_fixed:
    label: Fixed
    mode: boolean
    fixed: true
)");
  ASSERT_TRUE(config->LoadFromStream(yaml));
  engine->ApplySchema(new Schema("switch_test_menu", config));
  string committed;
  engine->sink().connect([&](const string& text) { committed += text; });
  const KeyEvent hotkey("Control+grave");
  auto open = [&] {
    EXPECT_TRUE(engine->ProcessKey(hotkey));
    EXPECT_NE(engine.get(), engine->active_engine());
    EXPECT_TRUE(engine->ProcessKey(hotkey));
    return engine->active_engine()->context();
  };
  engine->context()->set_input("abc");
  engine->context()->set_caret_pos(2);
  auto* menu = open();
  ASSERT_TRUE(menu->HasMenu());
  EXPECT_EQ("Input", menu->GetSelectedCandidate()->text());
  EXPECT_TRUE(engine->ProcessKey(hotkey));
  EXPECT_EQ(menu, engine->active_engine()->context());
  EXPECT_TRUE(menu->Select(0));
  EXPECT_TRUE(menu->Select(1));  // Declared order, not alphabetical.
  EXPECT_EQ(engine.get(), engine->active_engine());
  EXPECT_TRUE(engine->context()->setting_selected("a_input", "compact"));
  EXPECT_EQ("abc", engine->context()->input());
  EXPECT_EQ(2u, engine->context()->caret_pos());

  menu = open();
  EXPECT_TRUE(menu->Select(1));
  EXPECT_TRUE(menu->Select(0));
  EXPECT_TRUE(menu->Select(1));
  EXPECT_EQ((vector<string>{"a", "b"}),
            engine->context()->setting_values("b_fuzzy"));
  EXPECT_TRUE(menu->Select(0));
  EXPECT_TRUE(menu->Select(1));
  EXPECT_TRUE(engine->context()->setting_values("b_fuzzy").empty());
  EXPECT_TRUE(menu->Select(2));  // Back to group list.
  EXPECT_TRUE(menu->Select(2));  // Fixed group.
  EXPECT_TRUE(menu->Select(0));  // Cannot change fixed value.
  EXPECT_TRUE(engine->context()->setting_selected("c_fixed", "true"));
  EXPECT_TRUE(engine->ProcessKey(KeyEvent("Escape")));
  EXPECT_EQ(engine.get(), engine->active_engine());
  EXPECT_TRUE(committed.empty());
  EXPECT_EQ("abc", engine->context()->input());
  engine->context()->Clear();
  user->SetItem(path, nullptr);
  EXPECT_TRUE(user->Save());
}

TEST(SchemaSwitchesTest, ApiRejectsMissingSession) {
  EXPECT_FALSE(rime_get_api()->select_switch(0, "scoped_emoji", True));
}

}  // namespace
}  // namespace rime
