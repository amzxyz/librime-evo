//
// Copyright RIME Developers
// Distributed under the BSD License
//

#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <cstdio>

#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/registry.h>
#include <rime/schema.h>
#include <rime/translation.h>
#include <rime/translator.h>
#include <rime_api.h>

namespace rime {
namespace {

class InputTypesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    the<Config> user(Config::Require("user_config")->Create("user"));
    ASSERT_TRUE(
        user->SetString("var/type_groups/input_types_test/input_types", ""));
    engine_.reset(Engine::Create());
    auto* config = new Config;
    std::istringstream stream(R"yaml(
input_types:
  - {option: zrm, label: 自然码}
  - {option: pinyin, label: 全拼}
)yaml");
    ASSERT_TRUE(config->LoadFromStream(stream));
    engine_->ApplySchema(new Schema("input_types_test", config));
  }

  the<Engine> engine_;
};

TEST_F(InputTypesTest, DefaultsToFirstInputType) {
  EXPECT_FALSE(engine_->context()->get_option("pinyin"));
  EXPECT_TRUE(engine_->context()->get_option("zrm"));
}

TEST_F(InputTypesTest, SelectingInputTypeClearsOtherTypes) {
  engine_->context()->set_option("pinyin", true);

  EXPECT_TRUE(engine_->context()->get_option("pinyin"));
  EXPECT_FALSE(engine_->context()->get_option("zrm"));

  engine_->context()->set_option("zrm", true);

  EXPECT_FALSE(engine_->context()->get_option("pinyin"));
  EXPECT_TRUE(engine_->context()->get_option("zrm"));
}

class InputTypeQueryCounter : public Translator {
 public:
  explicit InputTypeQueryCounter(const Ticket& ticket) : Translator(ticket) {}

  an<Translation> Query(const string& input, const Segment& segment) override {
    ++queries;
    return New<UniqueTranslation>(
        New<SimpleCandidate>("test", segment.start, segment.end, input));
  }

  inline static size_t queries = 0;
};

TEST_F(InputTypesTest, SwitchingRecomposesOnceAfterTheRadioGroupSettles) {
  auto& registry = Registry::instance();
  registry.Register("input_type_counter", new Component<InputTypeQueryCounter>);
  auto* schema = engine_->schema();
  auto* config = schema->config();
  auto segmentors = New<ConfigList>();
  segmentors->Append(New<ConfigValue>("abc_segmentor"));
  config->SetItem("engine/segmentors", segmentors);
  auto translators = New<ConfigList>();
  translators->Append(New<ConfigValue>("input_type_counter"));
  config->SetItem("engine/translators", translators);
  engine_->ApplySchema(schema);
  engine_->context()->set_input("ni");
  InputTypeQueryCounter::queries = 0;
  engine_->context()->set_option("pinyin", true);
  EXPECT_EQ(1u, InputTypeQueryCounter::queries);
  EXPECT_TRUE(engine_->context()->get_option("pinyin"));
  EXPECT_FALSE(engine_->context()->get_option("zrm"));
  engine_.reset();
  registry.Unregister("input_type_counter");
}

TEST(TypeGroupsTest, TypeChoicesPersistButOrdinarySwitchesReset) {
  const string id = "type_reset_test";
  const string path = "var/type_groups/" + id;
  the<Config> user(Config::Require("user_config")->Create("user"));
  ASSERT_TRUE(user->SetItem(path, nullptr));
  auto schema = [&]() {
    auto config = new Config;
    std::istringstream data(R"YAML(input_types:
  - {option: pinyin}
  - {option: zrm}
aux_types: [{option: aux_wanxiang}, {option: aux_zrm}]
switches:
  - name: test_toggle
    reset: 1
    states: [off, on]
  - options: [test_radio_a, test_radio_b]
    reset: 1
    states: [A, B]
)YAML");
    EXPECT_TRUE(config->LoadFromStream(data));
    return new Schema(id, config);
  };
  the<Engine> engine(Engine::Create());
  engine->ApplySchema(schema());
  auto* context = engine->context();
  EXPECT_TRUE(context->get_option("pinyin"));
  EXPECT_TRUE(context->get_option("aux_wanxiang"));
  EXPECT_TRUE(context->get_option("test_toggle"));
  EXPECT_FALSE(context->get_option("test_radio_a"));
  EXPECT_TRUE(context->get_option("test_radio_b"));
  context->set_option("zrm", true);
  context->set_option("aux_zrm", true);
  context->set_option("test_toggle", false);
  context->set_option("test_radio_a", true);
  context->set_option("test_radio_b", false);
  engine->ApplySchema(schema());
  EXPECT_TRUE(context->get_option("zrm"));
  EXPECT_TRUE(context->get_option("aux_zrm"));
  EXPECT_TRUE(context->get_option("test_toggle"));
  EXPECT_FALSE(context->get_option("test_radio_a"));
  EXPECT_TRUE(context->get_option("test_radio_b"));
  ASSERT_TRUE(user->SetItem(path, nullptr));
  EXPECT_TRUE(user->Save());
}

TEST(TypeGroupsTest, RemovedChoiceFallsBackToFirstOption) {
  const string id = "removed_type_test";
  const string path = "var/type_groups/" + id + "/input_types";
  the<Config> user(Config::Require("user_config")->Create("user"));
  ASSERT_TRUE(user->SetString(path, "zrm"));
  auto config = new Config;
  std::istringstream data(R"YAML(input_types:
  - {option: pinyin}
  - {option: flypy}
)YAML");
  ASSERT_TRUE(config->LoadFromStream(data));
  the<Engine> engine(Engine::Create());
  engine->ApplySchema(new Schema(id, config));
  EXPECT_TRUE(engine->context()->get_option("pinyin"));
  EXPECT_EQ("pinyin", user->GetValue(path)->str());
  ASSERT_TRUE(user->SetItem(path, nullptr));
  EXPECT_TRUE(user->Save());
}

TEST(TypeGroupsTest, ApiEnumeratesDisplayReadyOptions) {
  const string id = "type_options_api_test";
  const string filename = id + ".schema.yaml";
  {
    std::ofstream file(filename);
    file << R"YAML(schema: {schema_id: type_options_api_test}
input_types:
  - {option: pinyin, label: 全拼}
  - {option: zrm, name: custom_prism}
aux_types:
  - {option: aux_wanxiang, label: 万象}
  - {option: aux_zrm, label: ""}
)YAML";
    ASSERT_TRUE(file.good());
  }
  auto api = rime_get_api();
  the<Config> user(Config::Require("user_config")->Create("user"));
  const string path = "var/type_groups/" + id;
  ASSERT_TRUE(user->SetItem(path, nullptr));
  ASSERT_TRUE(user->Save());
  RimeTypeOption* items = nullptr;
  size_t count = 0;
  ASSERT_TRUE(
      api->get_type_options(0, id.c_str(), "input_types", &items, &count));
  ASSERT_EQ(2u, count);
  EXPECT_STREQ("pinyin", items[0].option);
  EXPECT_STREQ("全拼", items[0].label);
  EXPECT_TRUE(items[0].selected);
  EXPECT_STREQ("zrm", items[1].label);
  EXPECT_FALSE(items[1].selected);
  api->free_type_options(items, count);
  ASSERT_TRUE(api->select_type_option(0, id.c_str(), "aux_types", "aux_zrm"));
  ASSERT_TRUE(
      api->get_type_options(0, id.c_str(), "aux_types", &items, &count));
  ASSERT_EQ(2u, count);
  EXPECT_STREQ("万象", items[0].label);
  EXPECT_FALSE(items[0].selected);
  EXPECT_STREQ("aux_zrm", items[1].label);
  EXPECT_TRUE(items[1].selected);
  api->free_type_options(items, count);
  EXPECT_FALSE(api->get_type_options(0, id.c_str(), "unknown", &items, &count));
  EXPECT_EQ(nullptr, items);
  EXPECT_EQ(0u, count);
  ASSERT_TRUE(user->SetItem(path, nullptr));
  EXPECT_TRUE(user->Save());
  std::remove(filename.c_str());
}

TEST(TypeGroupsTest, CandidateEditRejectsMissingTarget) {
  auto api = rime_get_api();
  EXPECT_FALSE(api->edit_candidate(0, 0, True, "manual_up"));
  const auto session = api->create_session();
  ASSERT_NE(0u, session);
  EXPECT_FALSE(api->edit_candidate(session, 0, True, "auto_delete"));
  EXPECT_FALSE(api->edit_candidate(session, 0, False, "manual_delete"));
  EXPECT_FALSE(api->edit_candidate(session, 0, False, "manual_down"));
  EXPECT_FALSE(api->edit_candidate(session, 0, False, "unknown"));
  EXPECT_FALSE(api->edit_candidate(session, 0, False, nullptr));
  api->destroy_session(session);
}

TEST(TypeGroupsTest, IndependentGroupsPersistAcrossSessions) {
  const string schema_id = "saved_type_groups_test";
  const string path = "var/type_groups/" + schema_id;
  the<Config> user(Config::Require("user_config")->Create("user"));
  ASSERT_TRUE(user->SetItem(path, nullptr));
  auto schema = [&]() {
    auto config = new Config;
    std::istringstream data(R"YAML(input_types:
  - {option: pinyin}
  - {option: flypy}
aux_types: [{option: aux_wanxiang}, {option: aux_zrm}]
)YAML");
    EXPECT_TRUE(config->LoadFromStream(data));
    return new Schema(schema_id, config);
  };
  {
    the<Engine> engine(Engine::Create());
    engine->ApplySchema(schema());
    EXPECT_TRUE(engine->context()->get_option("pinyin"));
    EXPECT_TRUE(engine->context()->get_option("aux_wanxiang"));
    engine->context()->set_option("flypy", true);
    engine->context()->set_option("aux_zrm", true);
    EXPECT_TRUE(engine->context()->get_option("flypy"));
    EXPECT_TRUE(engine->context()->get_option("aux_zrm"));
    EXPECT_FALSE(engine->context()->get_option("aux_wanxiang"));
  }
  {
    the<Engine> engine(Engine::Create());
    engine->ApplySchema(schema());
    EXPECT_TRUE(engine->context()->get_option("flypy"));
    EXPECT_TRUE(engine->context()->get_option("aux_zrm"));
    EXPECT_FALSE(engine->context()->get_option("pinyin"));
    EXPECT_FALSE(engine->context()->get_option("aux_wanxiang"));
    engine->context()->set_option("aux_zrm", false);
    EXPECT_TRUE(engine->context()->get_option("aux_zrm"));
  }
  ASSERT_TRUE(user->SetItem(path, nullptr));
  EXPECT_TRUE(user->Save());
}
}  // namespace
}  // namespace rime
