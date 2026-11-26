//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <gtest/gtest.h>
#include <rime/dict/rewrite_store.h>

#include <filesystem>
#include <fstream>

using namespace rime;

class RimeRewriteStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  void TearDown() override { std::filesystem::remove_all(root_); }

  const path root_{"rewrite_store_test"};
};

TEST_F(RimeRewriteStoreTest, OrderedStagesAndIncrementalBuild) {
  const path file = root_ / "test.rewrite.bin";
  const vector<RewriteStageId> ids{{1, 1}, {2, 2}, {1, 1}};
  int builds = 0;
  auto load = [&](size_t i, RewriteStageData* data) {
    ++builds;
    if (i == 1) {
      data->entries.push_back({"乙", "甲", ""});
    } else {
      data->entries.push_back({"甲", "乙", "raw"});
    }
    return true;
  };
  ASSERT_TRUE(RewriteStore::Build(file, ids, load));
  EXPECT_EQ(2, builds);
  ASSERT_TRUE(RewriteStore::Build(file, ids, load));
  EXPECT_EQ(2, builds);
  const path alias = root_ / "alias.rewrite.bin";
  ASSERT_TRUE(RewriteStore::Build(alias, ids, load, false, {file}));
  EXPECT_EQ(2, builds);
  EXPECT_TRUE(RewriteStore::Open(alias));
  {
    auto store = RewriteStore::Open(file);
    ASSERT_TRUE(store);
    EXPECT_EQ(3u, store->stage_count());
    vector<string> values;
    ASSERT_TRUE(store->Lookup("甲", &values));
    EXPECT_EQ((vector<string>{"乙"}), values);
    string first;
    ASSERT_TRUE(store->LookupFirst("甲", &first));
    EXPECT_EQ(values.front(), first);
    EXPECT_FALSE(store->LookupFirst("未命中", &first));
    EXPECT_TRUE(first.empty());
    vector<RewriteResult> rewritten;
    ASSERT_TRUE(store->LookupWithPreedit("甲", &rewritten));
    ASSERT_EQ(1u, rewritten.size());
    EXPECT_EQ("乙", rewritten[0].value);
    EXPECT_EQ("raw", rewritten[0].preedit);
    string output;
    vector<RewriteSentenceState> states;
    ASSERT_TRUE(store->ConvertSentence("甲", &output, &states));
    EXPECT_EQ("乙", output);
    ASSERT_TRUE(store->ConvertSentence("甲甲", &output, &states));
    EXPECT_EQ("乙乙", output);
    ASSERT_TRUE(store->ConvertSentence("甲甲", &output));
    EXPECT_EQ("乙乙", output);
  }
  const vector<RewriteStageId> changed{{1, 1}, {3, 3}};
  auto change = [&](size_t i, RewriteStageData* data) {
    EXPECT_EQ(1u, i);
    ++builds;
    data->entries = {{"乙", "丙", ""}};
    return true;
  };
  ASSERT_TRUE(RewriteStore::Build(file, changed, change));
  EXPECT_EQ(3, builds);
  auto store = RewriteStore::Open(file);
  ASSERT_TRUE(store);
  vector<string> values;
  ASSERT_TRUE(store->Lookup("甲", &values));
  EXPECT_EQ((vector<string>{"丙"}), values);
}

TEST_F(RimeRewriteStoreTest, FailedBuildPreservesExistingResource) {
  const path file = root_ / "test.rewrite.bin";
  const vector<RewriteStageId> ids{{1, 1}};
  ASSERT_TRUE(
      RewriteStore::Build(file, ids, [](size_t, RewriteStageData* data) {
        data->entries = {{"甲", "乙", ""}};
        return true;
      }));
  EXPECT_FALSE(RewriteStore::Build(
      file, {{2, 2}}, [](size_t, RewriteStageData*) { return false; }));
  {
    auto store = RewriteStore::Open(file);
    ASSERT_TRUE(store);
    vector<string> values;
    ASSERT_TRUE(store->Lookup("甲", &values));
    EXPECT_EQ((vector<string>{"乙"}), values);
  }
  std::ofstream(static_cast<const std::filesystem::path&>(file),
                std::ios::binary | std::ios::app)
      << "x";
  EXPECT_FALSE(RewriteStore::Open(file));
  ASSERT_TRUE(
      RewriteStore::Build(file, ids, [](size_t, RewriteStageData* data) {
        data->entries = {{"甲", "乙", ""}};
        return true;
      }));
  EXPECT_TRUE(RewriteStore::Open(file));
}

TEST_F(RimeRewriteStoreTest, InvalidTrieIsRebuilt) {
  const path file = root_ / "test.rewrite.bin";
  const vector<RewriteStageId> ids{{1, 1}};
  int builds = 0;
  auto load = [&](size_t, RewriteStageData* data) {
    ++builds;
    data->entries = {{"甲乙", "丙", ""}};
    return true;
  };
  ASSERT_TRUE(RewriteStore::Build(file, ids, load));
  {
    // First stage starts after the 16-byte header and its 32-byte index.
    // Compact stage header: u64 trie size, u32 page count, key count,
    // flags. The directory and character pages precede the trie.
    std::fstream stream(static_cast<const std::filesystem::path&>(file),
                        std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(stream);
    constexpr uint64_t stage_offset = 48;
    stream.seekg(stage_offset + 8);
    unsigned char bytes[4]{};
    ASSERT_TRUE(stream.read(reinterpret_cast<char*>(bytes), sizeof(bytes)));
    uint32_t pages = 0;
    for (size_t i = 0; i < sizeof(bytes); ++i)
      pages |= uint32_t{bytes[i]} << (8 * i);
    ASSERT_GT(pages, 0u);
    const uint64_t trie_offset = 20 + 8704 + uint64_t{pages} * 1024;
    stream.seekp(stage_offset + trie_offset);
    // An invalid root offset, with the resource header and fingerprint intact.
    const char invalid[] = {0, 0, char(0xff), char(0x7f)};
    ASSERT_TRUE(stream.write(invalid, sizeof(invalid)));
  }
  EXPECT_FALSE(RewriteStore::Open(file));
  ASSERT_TRUE(RewriteStore::Build(file, ids, load));
  EXPECT_EQ(2, builds);
  EXPECT_TRUE(RewriteStore::Open(file));
}

TEST_F(RimeRewriteStoreTest, CompactStagePreservesAllLookupPaths) {
  const path file = root_ / "test.rewrite.bin";
  const vector<RewriteStageId> ids{{1, 1}};
  auto load = [](size_t, RewriteStageData* data) {
    data->entries.push_back({"甲", "一", ""});
    data->entries.push_back({"甲乙", "二", "ab"});
    data->entries.push_back({"甲乙丙", "三", ""});
    data->entries.push_back({"甲乙丙", "叁", ""});
    data->entries.push_back({"甲乙丙丁", "四", ""});
    data->entries.push_back({"123", "数字", "abc"});
    return true;
  };
  ASSERT_TRUE(RewriteStore::Build(file, ids, load));
  {
    auto store = RewriteStore::Open(file);
    ASSERT_TRUE(store);
    vector<string> values;
    ASSERT_TRUE(store->Lookup("甲", &values));
    EXPECT_EQ((vector<string>{"一"}), values);
    ASSERT_TRUE(store->Lookup("甲乙丙", &values));
    EXPECT_EQ((vector<string>{"三", "叁"}), values);
    EXPECT_FALSE(store->Lookup("甲乙丙丁戊", &values));
    vector<RewriteResult> rewritten;
    ASSERT_TRUE(store->LookupWithPreedit("123", &rewritten));
    ASSERT_EQ(1u, rewritten.size());
    EXPECT_EQ("数字", rewritten[0].value);
    EXPECT_EQ("abc", rewritten[0].preedit);
    vector<RewriteSentenceState> states;
    vector<string> inputs;
    inputs.push_back("甲");
    inputs.push_back("甲乙");
    inputs.push_back("甲乙丙");
    inputs.push_back("甲乙丙丁");
    inputs.push_back("甲乙丙");
    inputs.push_back("甲乙戊甲");
    inputs.push_back("甲乙戊甲");
    for (const auto& input : inputs) {
      string full;
      string incremental;
      ASSERT_TRUE(store->ConvertSentence(input, &full));
      ASSERT_TRUE(store->ConvertSentence(input, &incremental, &states));
      EXPECT_EQ(full, incremental);
    }
    string output;
    ASSERT_TRUE(store->ConvertSentence("甲乙丙丁甲乙", &output));
    EXPECT_EQ("四二", output);
  }

  // Reject unknown flags rather than masking legacy bits.
  {
    std::fstream stream(static_cast<const std::filesystem::path&>(file),
                        std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(stream);
    stream.seekp(48 + 17);
    stream.put(1);
    ASSERT_TRUE(stream);
  }
  EXPECT_FALSE(RewriteStore::Open(file));
  ASSERT_TRUE(RewriteStore::Build(file, ids, load));
  EXPECT_TRUE(RewriteStore::Open(file));
}

TEST_F(RimeRewriteStoreTest, ExactKeyValueLookup) {
  const path file = root_ / "charset.rewrite.bin";
  ASSERT_TRUE(RewriteStore::Build(
      file, {{1, 1}}, [](size_t, RewriteStageData* data) {
        data->entries = {{"一", "Tabfghjtu", ""},
                         {"𰻞", "fgju", ""},
                         {"一丁", "phrase", ""},
                         {"一丁", "alternate", ""},
                         {"space", "a b", ""}};
        return true;
      }));
  auto store = RewriteStore::Open(file);
  ASSERT_TRUE(store);
  string value;
  ASSERT_TRUE(store->LookupFirst("一", &value));
  EXPECT_EQ("Tabfghjtu", value);
  ASSERT_TRUE(store->LookupFirst("𰻞", &value));
  EXPECT_EQ("fgju", value);
  ASSERT_TRUE(store->LookupFirst("space", &value));
  EXPECT_EQ("a b", value);
  vector<string> values;
  ASSERT_TRUE(store->Lookup("一丁", &values));
  ASSERT_EQ((vector<string>{"phrase", "alternate"}), values);
  ASSERT_TRUE(store->LookupFirst("一丁", &value));
  EXPECT_EQ(values.front(), value);
  EXPECT_FALSE(store->LookupFirst("一丁丁", &value));
  EXPECT_TRUE(value.empty());
  EXPECT_FALSE(store->LookupFirst("", &value));
  EXPECT_FALSE(store->LookupFirst("一", nullptr));
}

TEST_F(RimeRewriteStoreTest, RejectsPreviousFormatVersion) {
  const path file = root_ / "test.rewrite.bin";
  const vector<RewriteStageId> ids{{1, 1}};
  auto load = [](size_t, RewriteStageData* data) {
    data->entries = {{"甲", "乙", ""}};
    return true;
  };
  ASSERT_TRUE(RewriteStore::Build(file, ids, load));
  EXPECT_EQ(48u + 20u + 8704u + 1024u + 8u + 4u,
            std::filesystem::file_size(file));
  {
    std::fstream stream(static_cast<const std::filesystem::path&>(file),
                        std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(stream);
    stream.seekp(8);
    stream.put(1);
    ASSERT_TRUE(stream);
  }
  EXPECT_FALSE(RewriteStore::Open(file));
  ASSERT_TRUE(RewriteStore::Build(file, ids, load));
  EXPECT_TRUE(RewriteStore::Open(file));
}
