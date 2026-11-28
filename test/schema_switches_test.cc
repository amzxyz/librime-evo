#include <gtest/gtest.h>
#include <sstream>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
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
      $pinyin: Full
      $zrm: Double
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

TEST(SchemaSwitchesTest, FixedSettingPreservesSavedChoice) {
  Config config, user;
  std::istringstream yaml(R"(
runtime_settings:
  layout:
    mode: single
    default: full
    fixed: compact
    values:
      full: Full
      compact: Compact
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

TEST(SchemaSwitchesTest, ApiRejectsMissingSession) {
  EXPECT_FALSE(rime_get_api()->select_switch(0, "scoped_emoji", True));
}

}  // namespace
}  // namespace rime
