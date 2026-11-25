//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Reverse lookup must honor schema-declared syllable delimiters
// (`speller/delimiter`). Regression: ReverseLookupTranslator hardcoded an
// empty delimiter set, so input like "xi'an" could not be syllabified and
// the lookup listed no candidates at all.
//
#include <gtest/gtest.h>
#include <rime_api.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/prism.h>
#include <rime/dict/reverse_lookup_dictionary.h>
#include <rime/dict/table.h>
#include <rime/gear/reverse_lookup_weight.h>
#include <fstream>
#include <string>

namespace {

constexpr const char* kSchemaYaml = R"YAML(# encoding: utf-8
schema:
  schema_id: reverse_lookup_delimiter_test
  name: reverse lookup delimiter test
engine:
  processors: [recognizer, speller]
  segmentors: [matcher, abc_segmentor]
  translators: [reverse_lookup_translator]
speller:
  alphabet: "abcdefghijklmnopqrstuvwxyz'"
  delimiter: " '"
recognizer:
  patterns:
    reverse_lookup: "^`[a-z']*$"
reverse_lookup:
  dictionary: dictionary_test
  prefix: "`"
)YAML";

// Types |keys| in a fresh session and returns the menu texts joined by '|'.
std::string Candidates(const char* keys) {
  RimeApi* api = rime_get_api();
  RimeSessionId session = api->create_session();
  EXPECT_NE(0, session);
  EXPECT_TRUE(api->select_schema(session, "reverse_lookup_delimiter_test"));
  for (const char* p = keys; *p; ++p) {
    api->process_key(session, static_cast<unsigned char>(*p), 0);
  }
  std::string result;
  RIME_STRUCT(RimeContext, ctx);
  if (api->get_context(session, &ctx)) {
    for (int i = 0; i < ctx.menu.num_candidates; ++i) {
      if (i) {
        result += '|';
      }
      result += ctx.menu.candidates[i].text ? ctx.menu.candidates[i].text : "";
    }
    api->free_context(&ctx);
  }
  api->destroy_session(session);
  return result;
}

}  // namespace

#if !defined(_WIN32) || !defined(RIME_IMPORTS)
TEST(ReverseLookupDictionary, BuildsAndReloadsEntriesAndStems) {
  auto db = rime::New<rime::ReverseDb>(rime::path{"reverse_builder_test.bin"});
  db->Remove();
  rime::Syllabary syllabary{"a", "b", "c"};
  rime::Vocabulary vocabulary;
  rime::ShortDictEntry entry;
  entry.text = "shared";
  vocabulary[0].entries.push_back(entry);
  vocabulary[1].entries.push_back(entry);
  rime::ShortDictEntry other;
  other.text = "other";
  vocabulary[1].entries.push_back(other);
  rime::ReverseLookupTable stems;
  stems["shared"].insert("z");
  stems["shared"].insert("a");
  stems["stem-only"].insert("b");

  ASSERT_TRUE(db->Build(nullptr, syllabary, vocabulary, stems, 123));
  ASSERT_TRUE(db->Save());
  ASSERT_TRUE(db->Load());
  EXPECT_EQ(123u, db->dict_file_checksum());
  rime::ReverseLookupDictionary dictionary(db);
  rime::string result;
  ASSERT_TRUE(dictionary.ReverseLookup("shared", &result));
  EXPECT_EQ("a b", result);
  ASSERT_TRUE(dictionary.ReverseLookup("other", &result));
  EXPECT_EQ("b", result);
  ASSERT_TRUE(dictionary.LookupStems("shared", &result));
  EXPECT_EQ("a z", result);
  ASSERT_TRUE(dictionary.LookupStems("stem-only", &result));
  EXPECT_EQ("b", result);
  EXPECT_FALSE(dictionary.ReverseLookup("stem-only", &result));
  EXPECT_FALSE(dictionary.LookupStems("other", &result));
  db->Remove();
}

TEST(ReverseLookupDictionary, SortsReadingsAcrossEvoSupplementTables) {
  // The base and supplement tables can have different local syllable IDs.
  // Use their existing weights without looking up codes in a shared prism.
  auto primary = rime::New<rime::Table>(
      rime::path{"reverse_weight_evo_primary.table.bin"});
  auto supplement = rime::New<rime::Table>(
      rime::path{"reverse_weight_evo_supplement.table.bin"});
  primary->Remove();
  supplement->Remove();

  rime::Code ji_code;
  ji_code.push_back(0);
  rime::Code primary_qi_code;
  primary_qi_code.push_back(1);
  rime::Syllabary primary_syllables{"ji", "qi"};
  rime::Vocabulary primary_words;
  primary_words[0].entries.emplace_back("骑", ji_code, 1.0);
  // The weight of an unrelated homophone must not affect "骑".
  primary_words[1].entries.emplace_back("其", primary_qi_code, 999.0);
  ASSERT_TRUE(primary->Build(primary_syllables, primary_words, 2));
  ASSERT_TRUE(primary->Save());
  ASSERT_TRUE(primary->Load());

  rime::Code supplement_qi_code;
  supplement_qi_code.push_back(0);
  rime::Syllabary supplement_syllables{"qi", "xi"};
  rime::Vocabulary supplement_words;
  supplement_words[0].entries.emplace_back("骑", supplement_qi_code, 100.0);
  supplement_words[0].entries.emplace_back("其", supplement_qi_code, 200.0);
  ASSERT_TRUE(supplement->Build(supplement_syllables, supplement_words, 2));
  ASSERT_TRUE(supplement->Save());
  ASSERT_TRUE(supplement->Load());

  const rime::vector<rime::string> packs{"supplement"};
  const rime::vector<rime::of<rime::Table>> tables{primary, supplement};
  rime::Dictionary dictionary("reverse_lookup_weight_test", packs, tables,
                              nullptr);
  rime::string codes = "ji qi";
  rime::SortReverseLookupCodesByWeight("骑", &codes, &dictionary);
  EXPECT_EQ("qi ji", codes);

  codes = "ji xi qi";
  rime::SortReverseLookupCodesByWeight("骑", &codes, &dictionary);
  EXPECT_EQ("qi ji xi", codes);

  codes = "ji qi";
  rime::SortReverseLookupCodesByWeight("不存在", &codes, &dictionary);
  EXPECT_EQ("ji qi", codes);

  supplement->Close();
  primary->Close();
  supplement->Remove();
  primary->Remove();
}

TEST(ReverseLookupDictionary, BuildsCompactEntriesAndStems) {
  auto db = rime::New<rime::ReverseDb>(
      rime::path{"reverse_compact_builder_test.bin"});
  db->Remove();
  rime::Syllabary syllabary{"a", "b"};
  rime::CompactDictEntryList entries;
  entries.text = "sharedother";
  entries.entries = {{0, 6, 0, 1.0}, {0, 6, 1, 2.0}, {6, 5, 1, 3.0}};
  rime::ReverseLookupTable stems;
  stems["shared"].insert("z");

  ASSERT_TRUE(db->Build(nullptr, syllabary, entries, stems, 456));
  ASSERT_TRUE(db->Save());
  ASSERT_TRUE(db->Load());
  rime::ReverseLookupDictionary dictionary(db);
  rime::string result;
  ASSERT_TRUE(dictionary.ReverseLookup("shared", &result));
  EXPECT_EQ("a b", result);
  ASSERT_TRUE(dictionary.ReverseLookup("other", &result));
  EXPECT_EQ("b", result);
  ASSERT_TRUE(dictionary.LookupStems("shared", &result));
  EXPECT_EQ("z", result);
  db->Remove();
}
#endif

TEST(ReverseLookupTranslator, HonorsSchemaDelimiter) {
  // Compile the shared pinyin fixture (dictionary_test.dict.yaml contains
  // "西安 xi an") into the working directory, which the test environment
  // uses as the deployed resource root.
  rime::the<rime::Dictionary> dict(new rime::Dictionary(
      "dictionary_test", {},
      {rime::New<rime::Table>(rime::path{"dictionary_test.table.bin"})},
      rime::New<rime::Prism>(rime::path{"dictionary_test.prism.bin"})));
  dict->Remove();
  rime::DictCompiler compiler(dict.get());
  ASSERT_TRUE(compiler.Compile(rime::path()));
  ASSERT_TRUE(dict->Load());

  std::ofstream("reverse_lookup_delimiter_test.schema.yaml") << kSchemaYaml;

  EXPECT_NE(std::string::npos, Candidates("`xian").find("西安"));
  // Before the fix a delimiter in the input terminated the lookup.
  EXPECT_NE(std::string::npos, Candidates("`xi'an").find("西安"));
}
