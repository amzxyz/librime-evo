#include <gtest/gtest.h>
#include <sstream>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
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
