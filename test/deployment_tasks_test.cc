//
// Copyright RIME Developers
// Distributed under the BSD License
//

#include <gtest/gtest.h>
#include <rime/candidate.h>
#include <rime/component.h>
#include <rime/deployer.h>
#include <rime/dict/level_db.h>
#include <rime/dict/prism.h>
#include <rime/dict/user_db.h>
#include <rime/dict/rewrite_store.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/gear/charset_policy.h>
#include <rime/segmentation.h>
#include <rime/lever/build_compiler.h>
#include <rime/lever/deployment_tasks.h>
#include <rime/lever/user_dict_manager.h>
#include <rime/service.h>
#include <rime_api.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include "rime/config/config_component.h"
#include "rime/registry.h"

namespace fs = std::filesystem;
using namespace rime;

class RimeDeploymentTasksTest : public ::testing::Test {
 protected:
  RimeDeploymentTasksTest() = default;

  void CleanUp() {
    std::error_code ec;
    fs::remove("installation.yaml", ec);
  }
  virtual void SetUp() { CleanUp(); }
  virtual void TearDown() { CleanUp(); }
};

TEST_F(RimeDeploymentTasksTest, InstallationUpdate) {
  Registry& r = Registry::instance();
  r.Register("installation_update", new Component<InstallationUpdate>);
  // initialization
  string sync_dir;
  {
    rime::Deployer deployer;
    deployer.distribution_code_name = "rime-test";
    deployer.distribution_name = "rime-test";
    deployer.distribution_version = "1.0.0";
    EXPECT_TRUE(
        deployer.RunTask("installation_update", rime::TaskInitializer()));
    Config config;
    EXPECT_TRUE(config.LoadFromFile(path("installation.yaml")));
    EXPECT_EQ(config["distribution_code_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_version"].ToString(), "1.0.0");
    EXPECT_TRUE(config.HasKey("backup_config_files"));
    EXPECT_TRUE(config.HasKey("install_time"));
    EXPECT_TRUE(config.HasKey("installation_id"));
    EXPECT_TRUE(config.HasKey("rime_version"));
    EXPECT_TRUE(config.HasKey("sync_dir"));
    sync_dir = config["sync_dir"].ToString();
    EXPECT_EQ(path(sync_dir), path(".") / "sync");
    EXPECT_FALSE(config.HasKey("update_time"));
  }
  // update
  string update_time;
  {
    rime::Deployer deployer;
    deployer.distribution_code_name = "rime-test";
    deployer.distribution_name = "rime-test";
    deployer.distribution_version = "2.0.0";
    EXPECT_TRUE(
        deployer.RunTask("installation_update", rime::TaskInitializer()));
    Config config;
    EXPECT_TRUE(config.LoadFromFile(path("installation.yaml")));
    EXPECT_EQ(config["distribution_code_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_version"].ToString(), "2.0.0");
    EXPECT_TRUE(config.HasKey("backup_config_files"));
    EXPECT_TRUE(config.HasKey("install_time"));
    EXPECT_TRUE(config.HasKey("installation_id"));
    EXPECT_TRUE(config.HasKey("rime_version"));
    EXPECT_TRUE(config.HasKey("sync_dir"));
    EXPECT_EQ(config["sync_dir"].ToString(), sync_dir);
    EXPECT_TRUE(config.HasKey("update_time"));
    update_time = config["update_time"].ToString();
    EXPECT_TRUE(!update_time.empty());
  }
  // no update
  {
    rime::Deployer deployer;
    deployer.distribution_code_name = "rime-test";
    deployer.distribution_name = "rime-test";
    deployer.distribution_version = "2.0.0";
    EXPECT_TRUE(
        deployer.RunTask("installation_update", rime::TaskInitializer()));
    Config config;
    EXPECT_TRUE(config.LoadFromFile(path("installation.yaml")));
    EXPECT_EQ(config["update_time"].ToString(), update_time);
    EXPECT_TRUE(config.HasKey("sync_dir"));
    EXPECT_EQ(config["sync_dir"].ToString(), sync_dir);
  }
}

TEST_F(RimeDeploymentTasksTest, InstallationUpdateRecover) {
  Registry& r = Registry::instance();
  r.Register("installation_update", new Component<InstallationUpdate>);
  // write a corrupted installation.yaml
  {
    std::ofstream fout("installation.yaml");
    fout << "|\n  abc";  // root node is a scalar
    fout.close();
    Config config;
    config.LoadFromFile(path("installation.yaml"));
    EXPECT_TRUE(config.IsValue(""));
  }
  {
    rime::Deployer deployer;
    deployer.distribution_code_name = "rime-test";
    deployer.distribution_name = "rime-test";
    deployer.distribution_version = "2.0.0";
    EXPECT_TRUE(
        deployer.RunTask("installation_update", rime::TaskInitializer()));
    Config config;
    EXPECT_TRUE(config.LoadFromFile(path("installation.yaml")));
    EXPECT_EQ(config["distribution_code_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_version"].ToString(), "2.0.0");
    EXPECT_TRUE(config.HasKey("backup_config_files"));
    EXPECT_TRUE(config.HasKey("install_time"));
    EXPECT_TRUE(config.HasKey("installation_id"));
    EXPECT_TRUE(config.HasKey("rime_version"));
    EXPECT_TRUE(config.HasKey("sync_dir"));
    EXPECT_FALSE(config.HasKey("update_time"));  // as if it is newly installed
  }
}

class RimeWorkspacePrismTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fs::remove_all(root_);
    fs::create_directories(root_);
    Initialize(root_, root_ / "build");
    std::ofstream(root_ / "default.yaml") << R"YAML(config_version: "1"
schema_list:
  - schema: workspace_prism_main
)YAML";
    std::ofstream(root_ / "workspace_prism.dict.yaml") << R"YAML(---
name: workspace_prism
version: "1"
...
你	ni	100
)YAML";
    std::ofstream(root_ / "workspace_prism_main.schema.yaml") << R"YAML(schema:
  schema_id: workspace_prism_main
  version: "1"
  dependencies: [workspace_prism_sub]
build:
  dictionaries:
    workspace_prism:
      algebra:
        shared: [xform/^ni$/ab/]
)YAML";
  }

  void TearDown() override {
    Initialize(path{"."}, path{"."});
    fs::remove_all(root_);
  }

  static void Initialize(const path& data, const path& staging) {
    auto api = rime_get_api();
    api->finalize();
    const auto data_dir = data.to_utf8_string();
    const auto staging_dir = staging.to_utf8_string();
    RIME_STRUCT(RimeTraits, traits);
    traits.shared_data_dir = traits.user_data_dir = traits.prebuilt_data_dir =
        data_dir.c_str();
    traits.staging_dir = staging_dir.c_str();
    api->initialize(&traits);
  }

  void WriteSecondSchema(const char* shared_spelling) {
    std::ofstream(root_ / "workspace_prism_sub.schema.yaml") << R"YAML(schema:
  schema_id: workspace_prism_sub
  version: "1"
build:
  dictionaries:
    workspace_prism:
      algebra:
        standalone: [xform/^ni$/jt/]
        shared: [xform/^ni$/)YAML" << shared_spelling << "/]\n";
  }

  Deployer& deployer_ = Service::instance().deployer();
  const fs::path root_ = fs::absolute("workspace_prism_test");
};

TEST_F(RimeWorkspacePrismTest, SyncsOnlyReferencedNativeUserDictionaries) {
  const auto build = root_ / "build";
  fs::create_directories(build);
  std::ofstream(build / "default.yaml") << R"YAML(schema_list:
  - schema: one
  - schema: two
)YAML";
  std::ofstream(build / "one.schema.yaml") << R"YAML(engine:
  translators:
    - script_translator
    - name: table_translator@english
      when: ascii_mode
    - script_translator@disabled
    - lua_translator@statistics
translator:
  dictionary: shared.extra
english:
  dictionary: words
  user_dict: en
  user_dict_mode: manual
disabled:
  user_dict: disabled
  user_dict_mode: off
statistics:
  user_dict: stats
unused:
  user_dict: unused
)YAML";
  std::ofstream(build / "two.schema.yaml") << R"YAML(engine:
  translators:
    - r10n_translator@another
    - table_translator@missing
    - table_translator@plain
another:
  user_dict: shared
missing:
  user_dict: missing
plain:
  user_dict: plain
  db_class: plain_userdb
)YAML";

  using RankDb = UserDbWrapper<LevelDb>;
  const auto names = {"shared", "en", "stats", "disabled", "unused", "plain"};
  for (const string name : names) {
    RankDb db(root_ / (name + ".userdb"), name);
    ASSERT_TRUE(db.Open());
    if (name == "stats") {
      const string key = "statistics/day/20261126/commit_length/1 \t12345678";
      ASSERT_TRUE(db.Update(key, "c=12 d=0 t=0"));
    } else {
      ASSERT_TRUE(db.Update("ni \t你", "1,0,0,0,0"));
    }
    ASSERT_TRUE(db.Close());
  }

  Deployer sync_deployer;
  sync_deployer.user_data_dir = root_;
  sync_deployer.staging_dir = build;
  sync_deployer.prebuilt_data_dir = root_;
  sync_deployer.sync_dir = root_ / "sync";
  sync_deployer.user_id = "sync-test";
  UserDictManager manager(&sync_deployer);
  ASSERT_TRUE(manager.SynchronizeAll());
  const auto snapshots = sync_deployer.user_data_sync_dir();
  EXPECT_TRUE(fs::exists(snapshots / "shared.userdb.tsv"));
  EXPECT_TRUE(fs::exists(snapshots / "en.userdb.tsv"));
  for (const string name : {"stats", "disabled", "unused", "plain", "missing"})
    EXPECT_FALSE(fs::exists(snapshots / (name + ".userdb.tsv")));
  EXPECT_FALSE(manager.Synchronize("stats"));

  RankDb stats(root_ / "stats.userdb", "stats");
  ASSERT_TRUE(stats.OpenReadOnly());
  string value;
  ASSERT_TRUE(stats.Fetch("statistics/day/20261126/commit_length/1 \t12345678",
                          &value));
  EXPECT_EQ("c=12 d=0 t=0", value);
  ASSERT_TRUE(stats.Close());
}

TEST_F(RimeWorkspacePrismTest, SharesPrismOnlyWhenAlgebraMatches) {
  WriteSecondSchema("ab");
  WorkspaceUpdate update;
  ASSERT_TRUE(update.Run(&deployer_));

  const path shared_path =
      deployer_.staging_dir / "workspace_prism.shared.prism.bin";
  const path standalone_path =
      deployer_.staging_dir / "workspace_prism.standalone.prism.bin";
  ASSERT_TRUE(fs::exists(shared_path));
  EXPECT_FALSE(fs::exists(standalone_path));
  EXPECT_FALSE(
      fs::exists(deployer_.staging_dir / "workspace_prism_sub.schema.yaml"));

  std::ofstream(root_ / "default.custom.yaml") << R"YAML(patch:
  schema_list:
    - schema: workspace_prism_main
    - schema: workspace_prism_sub
)YAML";
  ASSERT_TRUE(update.Run(&deployer_));
  {
    Prism shared(shared_path);
    Prism standalone(standalone_path);
    ASSERT_TRUE(shared.Load());
    ASSERT_TRUE(standalone.Load());
    EXPECT_TRUE(shared.HasKey("ab"));
    EXPECT_TRUE(standalone.HasKey("jt"));
  }
  const auto last_build = fs::last_write_time(shared_path);
  std::ofstream(root_ / "workspace_prism_main.custom.yaml")
      << "patch:\n  menu/page_size: 7\n";
  ASSERT_TRUE(update.Run(&deployer_));
  EXPECT_EQ(last_build, fs::last_write_time(shared_path));

  const auto second_schema = root_ / "workspace_prism_sub.schema.yaml";
  const auto old_time = fs::last_write_time(second_schema);
  WriteSecondSchema("cd");
  fs::last_write_time(second_schema, old_time + std::chrono::seconds(2));
  EXPECT_FALSE(update.Run(&deployer_));
  EXPECT_EQ(last_build, fs::last_write_time(shared_path));
  Prism shared(shared_path);
  ASSERT_TRUE(shared.Load());
  EXPECT_TRUE(shared.HasKey("ab"));
  EXPECT_FALSE(shared.HasKey("cd"));
}

TEST_F(RimeWorkspacePrismTest, RewriteKeyValueResourceWithoutFilter) {
  std::ofstream(root_ / "charset.tsv") << "# charset\n一\tTabfghjtu\n";
  std::ofstream(root_ / "workspace_prism_main.custom.yaml") << R"YAML(patch:
  build/rewrites/charset:
    files: [charset.tsv]
)YAML";
  WorkspaceUpdate update;
  ASSERT_TRUE(update.Run(&deployer_));
  ASSERT_TRUE(fs::exists(deployer_.staging_dir / "charset.rewrite.bin"));
  EXPECT_FALSE(RewriteResource::Require("../charset"));
  auto resource = RewriteResource::Require("charset");
  ASSERT_TRUE(resource);
  EXPECT_EQ(resource, RewriteResource::Require("charset"));
  string value;
  ASSERT_TRUE(resource->LookupFirst("一", &value));
  EXPECT_EQ("Tabfghjtu", value);
  vector<string> values;
  ASSERT_TRUE(resource->Lookup("一", &values));
  EXPECT_EQ((vector<string>{"Tabfghjtu"}), values);
  EXPECT_FALSE(resource->LookupFirst("丁", &value));
  EXPECT_TRUE(value.empty());

  std::ofstream(root_ / "charset.tsv") << "一\tfgju\n";
  ASSERT_TRUE(update.Run(&deployer_));
  ASSERT_TRUE(resource->LookupFirst("一", &value));
  EXPECT_EQ("fgju", value);
}

TEST_F(RimeWorkspacePrismTest, CharsetPolicyRulesAndExemptions) {
  std::ofstream(root_ / "charset.tsv") << "你\ta\n妳\tT\n𰻞\tT\n";
  std::ofstream(root_ / "workspace_prism_main.custom.yaml") << R"YAML(patch:
  build/rewrites/charset:
    files: [charset.tsv]
)YAML";
  ASSERT_TRUE(WorkspaceUpdate().Run(&deployer_));
  std::ofstream(root_ / "policy.yaml") << R"YAML(charset:
  data: charset
  exclude_tags: [reverse]
  rules:
    - option: small
      base: a
      addlist: ["𰻞"]
    - options: [traditional, alternate]
      base: T
      blacklist: ["𰻞"]
missing:
  data: missing_charset
  rules:
    - option: true
      base: a
)YAML";
  Config config;
  ASSERT_TRUE(config.LoadFromFile(root_ / "policy.yaml"));
  auto policy = CharsetPolicy::Load(&config, "charset");
  ASSERT_TRUE(policy);
  Context context;
  EXPECT_FALSE(policy->Bind(&context));
  context.set_option("small", true);
  auto checker = policy->Bind(&context);
  ASSERT_TRUE(checker);
  EXPECT_TRUE(checker("你"));
  EXPECT_TRUE(checker("𰻞"));
  EXPECT_FALSE(checker("妳"));
  EXPECT_FALSE(checker("丁"));
  EXPECT_TRUE(checker("妳好"));
  EXPECT_TRUE(checker("abc"));
  EXPECT_TRUE(checker("😊"));
  context.set_option("alternate", true);
  auto combined = policy->Bind(&context);
  ASSERT_TRUE(combined);
  EXPECT_TRUE(combined("你"));
  EXPECT_TRUE(combined("妳"));
  EXPECT_FALSE(combined("𰻞"));
  EXPECT_TRUE(checker("𰻞"));  // Existing queries retain their rule snapshot.
  Segment reverse;
  reverse.tags.insert("reverse");
  EXPECT_FALSE(policy->Bind(&context, &reverse));
  auto missing = CharsetPolicy::Load(&config, "missing");
  ASSERT_TRUE(missing);
  EXPECT_FALSE(missing->Bind(&context));
}

TEST_F(RimeWorkspacePrismTest, CharsetFiltersBeforeScriptSentenceSearch) {
  std::ofstream(root_ / "workspace_prism.dict.yaml") << R"YAML(---
name: workspace_prism
version: "1"
...
妳	ni	1000
你	ni	100
)YAML";
  std::ofstream(root_ / "charset.tsv") << "你\ta\n妳\tT\n";
  std::ofstream(root_ / "workspace_prism_main.custom.yaml") << R"YAML(patch:
  engine/processors: [speller, selector, express_editor]
  engine/segmentors: [abc_segmentor]
  engine/translators: [script_translator]
  translator:
    dictionary: workspace_prism
    prism: shared
    user_dict_mode: off
    charset: charset
    max_homophones: 1
    enable_completion: false
  build/rewrites/charset:
    files: [charset.tsv]
  charset:
    data: charset
    rules:
      - option: small
        base: a
)YAML";
  ASSERT_TRUE(WorkspaceUpdate().Run(&deployer_));
  auto api = rime_get_api();
  auto session = api->create_session();
  ASSERT_NE(0, session);
  ASSERT_TRUE(api->select_schema(session, "workspace_prism_main"));
  api->set_option(session, "small", true);
  ASSERT_TRUE(api->process_key(session, 'a', 0));
  ASSERT_TRUE(api->process_key(session, 'b', 0));
  auto* context = Service::instance().GetSession(session)->context();
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("你", context->GetSelectedCandidate()->text());
  ASSERT_TRUE(api->process_key(session, 'a', 0));
  ASSERT_TRUE(api->process_key(session, 'b', 0));
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("你你", context->GetSelectedCandidate()->text());
  api->set_option(session, "small", false);
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("妳妳", context->GetSelectedCandidate()->text());
  api->destroy_session(session);
}

TEST_F(RimeWorkspacePrismTest, DeploysAndSwitchesConditionalRewriter) {
  std::ofstream(root_ / "rewrite.tsv") << "你\t妳\n";
  std::ofstream(root_ / "workspace_prism_main.custom.yaml") << R"YAML(patch:
  engine/processors: [speller, selector, express_editor]
  engine/segmentors: [abc_segmentor]
  engine/translators: [table_translator]
  engine/filters:
    - {name: rewriter@rewrite_test, when: rewrite_on}
  translator:
    dictionary: workspace_prism
    prism: shared
    user_dict_mode: off
  build/rewrites/rewrite_test:
    files: [rewrite.tsv]
  rewrite_test:
    data: rewrite_test
    option: true
    mode: replace
)YAML";
  WorkspaceUpdate update;
  ASSERT_TRUE(update.Run(&deployer_));
  const path output = deployer_.staging_dir / "rewrite_test.rewrite.bin";
  ASSERT_TRUE(fs::exists(output));
  const auto built = fs::last_write_time(output);
  ASSERT_TRUE(update.Run(&deployer_));
  EXPECT_EQ(built, fs::last_write_time(output));

  auto api = rime_get_api();
  auto session = api->create_session();
  ASSERT_NE(0, session);
  ASSERT_TRUE(api->select_schema(session, "workspace_prism_main"));
  ASSERT_TRUE(api->process_key(session, 'a', 0));
  ASSERT_TRUE(api->process_key(session, 'b', 0));
  auto* context = Service::instance().GetSession(session)->context();
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("你", context->GetSelectedCandidate()->text());
  api->set_option(session, "rewrite_on", true);
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("妳", context->GetSelectedCandidate()->text());
  api->set_option(session, "rewrite_on", false);
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("你", context->GetSelectedCandidate()->text());
  api->destroy_session(session);
}

namespace {

class ProbeBuildCompiler : public BuildCompiler {
 public:
  explicit ProbeBuildCompiler(Deployer* deployer) : deployer_(deployer) {}

  bool Collect(const BuildRequest& request, BuildPlan* plan) override {
    string value;
    if (!request.config->GetString("build/probe/value", &value))
      return false;
    const path target = deployer_->staging_dir / "probe.bin";
    const path table = deployer_->staging_dir / "workspace_prism.table.bin";
    const string id = "probe:" + request.schema_id;
    CompileUnit unit(id, 1024, [target, table, value]() {
      if (!fs::exists(table))
        return false;
      std::ofstream out(static_cast<const fs::path&>(target));
      out << value;
      out.close();
      return bool(out);
    });
    unit.AddOutput("probe.bin");
    plan->AddUnit(std::move(unit), request.schema_id);
    plan->AddDependency("dictionary:workspace_prism", id);
    return true;
  }

 private:
  Deployer* deployer_;
};

}  // namespace

static void rime_deploy_probe_initialize() {
  Registry::instance().Register("build/probe",
                                new Component<ProbeBuildCompiler>);
}

static void rime_deploy_probe_finalize() {}

RIME_REGISTER_DEPLOYER_MODULE(deploy_probe)

TEST_F(RimeWorkspacePrismTest, RegisteredBuildCompilerWorksInBothDeployments) {
  EXPECT_FALSE(BuildCompiler::Require("build/probe"));
  std::ofstream(root_ / "workspace_prism_main.custom.yaml") << R"YAML(patch:
  build/probe:
    value: ready
)YAML";
  WorkspaceUpdate workspace;
  ASSERT_TRUE(workspace.Run(&deployer_));
  ASSERT_TRUE(BuildCompiler::Require("build/probe"));
  const path output = deployer_.staging_dir / "probe.bin";
  ASSERT_TRUE(fs::exists(output));
  EXPECT_TRUE(fs::remove(output));

  SchemaUpdate schema(path(root_ / "workspace_prism_main.schema.yaml"));
  ASSERT_TRUE(schema.Run(&deployer_));
  std::ifstream in(static_cast<const fs::path&>(output));
  string value;
  ASSERT_TRUE(in >> value);
  EXPECT_EQ("ready", value);
}

TEST_F(RimeWorkspacePrismTest, SharesNamedRewritesAndRejectsConflictingData) {
  WriteSecondSchema("ab");
  std::ofstream(root_ / "default.custom.yaml") << R"YAML(patch:
  schema_list:
    - schema: workspace_prism_main
    - schema: workspace_prism_sub
)YAML";
  std::ofstream(root_ / "rewrite.tsv") << "你\t妳\n";
  std::ofstream(root_ / "other.tsv") << "你\t他\n";
  std::ofstream(root_ / "workspace_prism_main.custom.yaml") << R"YAML(patch:
  build/rewrites/shared:
    files: [rewrite.tsv]
  build/rewrites/alias:
    files: [rewrite.tsv]
)YAML";
  const auto sub = root_ / "workspace_prism_sub.custom.yaml";
  std::ofstream(sub) << R"YAML(patch:
  build/rewrites/shared:
    files: [rewrite.tsv]
)YAML";
  WorkspaceUpdate update;
  ASSERT_TRUE(update.Run(&deployer_));
  const path shared = deployer_.staging_dir / "shared.rewrite.bin";
  const path alias = deployer_.staging_dir / "alias.rewrite.bin";
  ASSERT_TRUE(fs::exists(shared));
  ASSERT_TRUE(fs::exists(alias));
  const auto built = fs::last_write_time(shared);
  const auto alias_built = fs::last_write_time(alias);
  ASSERT_TRUE(update.Run(&deployer_));
  EXPECT_EQ(built, fs::last_write_time(shared));
  EXPECT_EQ(alias_built, fs::last_write_time(alias));

  const auto old_time = fs::last_write_time(sub);
  std::ofstream(sub) << R"YAML(patch:
  build/rewrites/shared:
    files: [other.tsv]
)YAML";
  fs::last_write_time(sub, old_time + std::chrono::seconds(2));
  EXPECT_FALSE(update.Run(&deployer_));
  EXPECT_EQ(built, fs::last_write_time(shared));
  EXPECT_EQ(alias_built, fs::last_write_time(alias));
}

TEST_F(RimeWorkspacePrismTest, NotifiesOnceForWholeSchemaBatch) {
  WriteSecondSchema("ab");
  std::ofstream(root_ / "default.custom.yaml") << R"YAML(patch:
  schema_list:
    - schema: workspace_prism_main
    - schema: workspace_prism_sub
)YAML";
  int before = 0;
  int after = 0;
  const vector<string> ids{"workspace_prism_main", "workspace_prism_sub"};
  const set<string> expected(ids.begin(), ids.end());
  auto prepare = [&](Deployer* deployer, const vector<string>& schemas) {
    EXPECT_EQ(&deployer_, deployer);
    EXPECT_EQ(ids, schemas);
    ++before;
    const path table = deployer_.staging_dir / "workspace_prism.table.bin";
    EXPECT_FALSE(fs::exists(table));
    std::ofstream(root_ / "workspace_prism_main.custom.yaml")
        << "patch:\n  build/probe:\n    value: prepared\n";
  };
  auto complete = [&](Deployer* deployer, const DeploymentResult& result) {
    EXPECT_EQ(&deployer_, deployer);
    ++after;
    EXPECT_TRUE(result.success);
    EXPECT_EQ(ids, result.schema_ids);
    EXPECT_EQ(expected, result.successful_schemas);
    EXPECT_TRUE(result.failed_schemas.empty());
    EXPECT_TRUE(fs::exists(deployer_.staging_dir / "probe.bin"));
    const path prism =
        deployer_.staging_dir / "workspace_prism.standalone.prism.bin";
    EXPECT_TRUE(fs::exists(prism));
  };
  boost::signals2::scoped_connection before_connection(
      deployer_.before_deploy().connect(prepare));
  boost::signals2::scoped_connection after_connection(
      deployer_.after_deploy().connect(complete));
  EXPECT_TRUE(WorkspaceUpdate().Run(&deployer_));
  EXPECT_EQ(1, before);
  EXPECT_EQ(1, after);
}

TEST(RimeDeploymentLifecycleTest, FailureAndObserverExceptionsAreReported) {
  Deployer deployer;
  const vector<string> ids{"first", "second"};
  auto select = [&](vector<string>* schemas) {
    *schemas = ids;
    return true;
  };
  int observers = 0;
  int work = 0;
  int after = 0;
  boost::signals2::scoped_connection throwing(
      deployer.before_deploy().connect([](Deployer*, const vector<string>&) {
        throw std::runtime_error("preparation failed");
      }));
  const set<string> expected(ids.begin(), ids.end());
  auto observe = [&](Deployer*, const vector<string>&) { ++observers; };
  auto finish = [&](Deployer*, const DeploymentResult& result) {
    ++after;
    EXPECT_FALSE(result.success);
    EXPECT_EQ(ids, result.schema_ids);
    EXPECT_TRUE(result.successful_schemas.empty());
    EXPECT_EQ(expected, result.failed_schemas);
  };
  boost::signals2::scoped_connection observer(
      deployer.before_deploy().connect(observe));
  boost::signals2::scoped_connection complete(
      deployer.after_deploy().connect(finish));
  EXPECT_FALSE(deployer.RunSchemaDeployment(select, [&](DeploymentResult*) {
    ++work;
    return true;
  }));
  EXPECT_EQ(0, work);
  EXPECT_EQ(1, observers);
  EXPECT_EQ(1, after);
  throwing.disconnect();
  EXPECT_FALSE(deployer.RunSchemaDeployment(select, [&](DeploymentResult*) {
    ++work;
    EXPECT_FALSE(deployer.RunSchemaDeployment(
        select, [](DeploymentResult*) { return true; }));
    return false;
  }));
  EXPECT_EQ(1, work);
  EXPECT_EQ(2, observers);
  EXPECT_EQ(2, after);
}

TEST(RimeBuildPlanTest, ResourceAliasesResolveToOneUnit) {
  Deployer deployer;
  BuildPlan plan(&deployer);
  int runs = 0;
  CompileUnit first("rewrite:emoji", 1, [&] { return ++runs == 1; });
  const auto index = plan.AddUnit(std::move(first), "first");
  ASSERT_TRUE(plan.AddAlias("rewrite:expression", index));
  plan.AddOwner(index, "second");
  plan.AddDependency("rewrite:emoji", "rewrite:expression");
  CompileUnit probe("probe", 1, [&] { return ++runs == 2; });
  plan.AddUnit(std::move(probe), "second");
  plan.AddDependency("rewrite:expression", "probe");
  set<string> failed;
  ASSERT_TRUE(plan.Run(&failed));
  EXPECT_TRUE(failed.empty());
  EXPECT_EQ(2, runs);
}

TEST_F(RimeWorkspacePrismTest, ForcedCompilationRebuildsAuxiliaryGroups) {
  std::ofstream(root_ / "aux.yaml") << "你\trx\n";
  std::ofstream(root_ / "workspace_prism_main.custom.yaml") << R"YAML(patch:
  build/auxiliary:
    dictionary: aux
    algebra:
      direct: ["aux|^([a-z]+)$|$1|"]
      suffix: ["aux|^([a-z]+)$|$1/|"]
)YAML";
  ASSERT_TRUE(WorkspaceUpdate().Run(&deployer_));
  const path direct = deployer_.staging_dir / "direct.aux.bin";
  const path suffix = deployer_.staging_dir / "suffix.aux.bin";
  ASSERT_TRUE(fs::exists(direct));
  ASSERT_TRUE(fs::exists(suffix));

  const auto past = fs::last_write_time(direct) - std::chrono::hours(24);
  fs::last_write_time(direct, past);
  fs::last_write_time(suffix, past);
  const auto direct_time = fs::last_write_time(direct);
  const auto suffix_time = fs::last_write_time(suffix);

  ASSERT_TRUE(WorkspaceUpdate().Run(&deployer_));
  EXPECT_EQ(direct_time, fs::last_write_time(direct));
  EXPECT_EQ(suffix_time, fs::last_write_time(suffix));

  SchemaUpdate update(path(root_ / "workspace_prism_main.schema.yaml"));
  update.set_verbose(true);
  ASSERT_TRUE(update.Run(&deployer_));
  EXPECT_GT(fs::last_write_time(direct), direct_time);
  EXPECT_GT(fs::last_write_time(suffix), suffix_time);
}

TEST_F(RimeWorkspacePrismTest, RejectsConflictingAuxiliaryGroups) {
  WriteSecondSchema("ab");
  std::ofstream(root_ / "default.custom.yaml") << R"YAML(patch:
  schema_list:
    - schema: workspace_prism_main
    - schema: workspace_prism_sub
)YAML";
  std::ofstream(root_ / "workspace_prism_main.custom.yaml") << R"YAML(patch:
  build/auxiliary:
    dictionary: aux
    algebra:
      test: [xform/a/b/]
)YAML";
  std::ofstream(root_ / "workspace_prism_sub.custom.yaml") << R"YAML(patch:
  build/auxiliary:
    dictionary: aux
    algebra:
      test: [xform/a/c/]
)YAML";
  EXPECT_FALSE(WorkspaceUpdate().Run(&deployer_));
  EXPECT_FALSE(fs::exists(deployer_.staging_dir / "test.aux.bin"));
}

TEST_F(RimeWorkspacePrismTest, FailedConfigBuildIsReported) {
  std::ofstream(root_ / "workspace_prism_main.custom.yaml") << R"YAML(patch:
  build/dictionaries/workspace_prism:
    __include: missing_config:/missing
)YAML";
  int notifications = 0;
  auto complete = [&](Deployer*, const DeploymentResult& result) {
    ++notifications;
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.successful_schemas.empty());
    EXPECT_EQ((set<string>{"workspace_prism_main"}), result.failed_schemas);
  };
  boost::signals2::scoped_connection connection(
      deployer_.after_deploy().connect(complete));
  EXPECT_FALSE(WorkspaceUpdate().Run(&deployer_));
  EXPECT_EQ(1, notifications);
  EXPECT_FALSE(fs::exists(deployer_.staging_dir / "workspace_prism.table.bin"));
}

TEST_F(RimeWorkspacePrismTest, ReentryCannotModifyWorkspace) {
  auto prepare = [&](Deployer*, const vector<string>&) {
    const path compiled = deployer_.staging_dir / "default.yaml";
    ASSERT_TRUE(fs::remove(compiled));
    EXPECT_FALSE(WorkspaceUpdate().Run(&deployer_));
    EXPECT_FALSE(fs::exists(compiled));
  };
  boost::signals2::scoped_connection connection(
      deployer_.before_deploy().connect(prepare));
  EXPECT_TRUE(WorkspaceUpdate().Run(&deployer_));
}

TEST(RimeBuildPlanTest, SkippedResourcesFailTheirOwningSchemas) {
  Deployer deployer;
  BuildPlan plan(&deployer);
  plan.AddUnit(CompileUnit("broken", 1, [] { return false; }), "first");
  bool ran = false;
  plan.AddUnit(CompileUnit("dependent", 1,
                           [&] {
                             ran = true;
                             return true;
                           }),
               "second");
  plan.AddDependency("broken", "dependent");
  set<string> failed;
  ASSERT_TRUE(plan.Run(&failed));
  EXPECT_FALSE(ran);
  EXPECT_EQ((set<string>{"first", "second"}), failed);
}
