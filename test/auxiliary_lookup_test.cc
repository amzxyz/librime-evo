// Copyright RIME Developers
// Distributed under the BSD License
//
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <gtest/gtest.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/candidate.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/menu.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/auxiliary_db.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/text_db.h>
#include <rime/dict/level_db.h>
#include <rime/dict/user_dictionary.h>
#include <rime/gear/auxiliary_matcher.h>
#include <rime/gear/translator_commons.h>

using namespace rime;

class RimeAuxiliaryLookupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream("aux_path_test.dict.yaml") << R"YAML(---
name: aux_path_test
version: "1.0"
sort: by_weight
...
你	ni	100
拟	ni	80
泥	ni	50
逆	ni	20
好	hao	100
蒿	hao	10
在	zai	100
哪	na	100
里	li	100
鳞	lin	100
𤆄	lin	100
增	zeng	100
锃	zeng	100000000
加	jia	100
哀	ai	100
爱	ai	10
最	zui	241723
嘴	zui	10503
快	kuai	93223
的	de	48288412
最快	zui kuai	18413
嘴的	zui de	933
甲	ab	10000000
乙	cd	10000
丙	a	100
甲乙	ab cd	100
丙乙	a cd	100
你好	ni hao	100
拟好	ni hao	10
泥蒿	ni hao	5
你在	ni zai	100
逆鳞	ni lin	100
你𤆄	ni lin	1000
增加	zeng jia	1000
增价	zeng jia	1
你好在哪	ni hao zai na	100
你在哪里	ni zai na li	100
拟在哪里	ni zai na li	10
你好在哪里	ni hao zai na li	100
拟好在哪里	ni hao zai na li	10
)YAML";
    std::ofstream("aux_path_test_aux.yaml") << R"YAML(你	rx
你	abcdef
你	weak
拟	norm
逆	zn
锃	jw
哀	wy
里	zt
嘴	ky
嘴	kuai
丙	B
)YAML";
    std::ofstream("aux_path_test.schema.yaml") << R"YAML(build:
  dictionaries:
    aux_path_test:
      algebra:
        pinyin: []
        zrm:
          - xform/^hao$/hk/
          - xform/^zai$/zl/
          - xform/^lin$/ln/
          - xform/^zeng$/zg/
          - xform/^jia$/jw/
          - xform/^zui$/zv/
          - xform/^kuai$/ky/
  auxiliary:
    dictionary: aux_path_test_aux
    algebra:
      aux_path_test:
        - "aux|^([a-z])[a-z]*$|$1|"
        - "aux|^([a-z]{2,})$|$1|abbrev"
        - "aux|^([a-z]{2,})$|$1/|"
        - "aux|^weak$|xy|abbrev"
        - "aux|^norm$|xy|"
        - "aux|^B$|b|abbrev"
      aux_path_other:
        - "aux|^([a-z])[a-z]*$|$1|"
        - "aux|^([a-z]{2,})$|$1/|"
)YAML";
    ASSERT_TRUE(
        DictCompiler::CompileAuxiliary(path{"aux_path_test.schema.yaml"}));
    ASSERT_TRUE(
        DictCompiler::CompileDictionaries(path{"aux_path_test.schema.yaml"}));
    database_.reset(new AuxiliaryDb(path{"aux_path_test.aux.bin"}));
    ASSERT_TRUE(database_->Load());
  }

  void TearDown() override {
    database_.reset();
    std::filesystem::remove_all("aux_path_cache.userdb");
    std::filesystem::remove_all("aux_path_sentence.userdb");
    std::filesystem::remove_all("aux_path_prefix.userdb");
    std::filesystem::remove_all("aux_path_word.userdb");
    for (const char* file :
         {"aux_path_test.dict.yaml", "aux_path_test_aux.yaml",
          "aux_path_test.schema.yaml", "aux_path_test.table.bin",
          "aux_path_test.reverse.bin", "aux_path_test.pinyin.prism.bin",
          "aux_path_test.zrm.prism.bin", "aux_path_test.aux.bin",
          "aux_path_other.aux.bin", "auto.aux.bin", "aux_path_user.txt"})
      std::remove(file);
  }

  an<Dictionary> OpenDictionary(const string& type, bool load = true) {
    auto dict = New<Dictionary>(
        "aux_path_test", vector<string>{},
        vector<of<Table>>{New<Table>(path{"aux_path_test.table.bin"})},
        New<Prism>(path{"aux_path_test." + type + ".prism.bin"}));
    if (load) {
      EXPECT_TRUE(dict->Load());
    }
    return dict;
  }

  vector<an<DictEntry>> Lookup(const string& input,
                               const string& type = "zrm",
                               bool enable_aux = true,
                               bool completion = true,
                               bool predict_word = false) {
    auto dict = OpenDictionary(type);
    AuxiliaryMatcher matcher(dict.get(), database_.get());
    Syllabifier syllabifier(" '", completion);
    if (enable_aux) {
      syllabifier.SetAuxiliaryLookup(
          [&matcher](const string& code, vector<AuxGraphMatch>* matches) {
            matcher.Lookup(code, matches);
          });
    }
    SyllableGraph graph;
    syllabifier.BuildSyllableGraph(input, *dict->prism(), &graph);
    auto result = dict->Lookup(graph, 0, nullptr, predict_word);
    vector<an<DictEntry>> entries;
    if (!result)
      return entries;
    for (auto bucket = result->rbegin(); bucket != result->rend(); ++bucket) {
      auto& it = bucket->second;
      it.ApplyAuxiliaryOrdering();
      while (!it.exhausted()) {
        entries.push_back(it.Peek());
        it.Next();
      }
    }
    return entries;
  }

  static size_t Position(const vector<an<DictEntry>>& entries,
                         const string& text) {
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i]->text == text)
        return i;
    }
    return entries.size();
  }

  the<AuxiliaryDb> database_;
};

TEST_F(RimeAuxiliaryLookupTest,
       CompilesEveryDeclaredGroupWithoutTranslatorSelection) {
  vector<AuxiliaryCharacter> characters;
  ASSERT_TRUE(database_->Lookup("rx", &characters));
  ASSERT_EQ(1u, characters.size());
  EXPECT_EQ(kAbbreviation, characters.front().type);

  AuxiliaryDb other(path{"aux_path_other.aux.bin"});
  ASSERT_TRUE(other.Load());
  EXPECT_FALSE(other.Lookup("rx", &characters));
  ASSERT_TRUE(other.Lookup("rx/", &characters));
  ASSERT_EQ(1u, characters.size());
  EXPECT_EQ("你", characters.front().text);
  EXPECT_EQ(kNormalSpelling, characters.front().type);
  vector<AuxiliaryCode> codes;
  ASSERT_TRUE(other.ReverseLookup("你", &codes));
  EXPECT_FALSE(std::any_of(codes.begin(), codes.end(),
                           [](const auto& code) { return code.code == "rx"; }));
}

TEST_F(RimeAuxiliaryLookupTest, DisplayCodesAreExplicitAndGroupSpecific) {
  Config schema;
  ASSERT_TRUE(schema.LoadFromFile(path{"aux_path_test.schema.yaml"}));
  auto rules = schema.GetList("build/auxiliary/algebra/aux_path_test");
  ASSERT_TRUE(rules->Append(New<ConfigValue>("aux|^rx$|rx|abbrev,display")));
  ASSERT_TRUE(rules->Append(New<ConfigValue>("aux|^abcdef$|abcdef|display")));
  ASSERT_TRUE(rules->Append(New<ConfigValue>("aux|^rx$|rzz|")));
  ASSERT_TRUE(schema.SaveToFile(path{"aux_path_test.schema.yaml"}));
  database_->Close();
  ASSERT_TRUE(
      DictCompiler::CompileAuxiliary(path{"aux_path_test.schema.yaml"}));

  auto display = AuxiliaryDb::Open("aux_path_test");
  ASSERT_TRUE(display);
  string code;
  ASSERT_TRUE(display->LookupDisplay("你", &code));
  EXPECT_EQ("abcdef rx", code);
  EXPECT_FALSE(display->LookupDisplay("拟", &code));
  EXPECT_TRUE(code.empty());

  vector<AuxiliaryCharacter> hits;
  ASSERT_TRUE(display->Lookup("rx", &hits));
  ASSERT_EQ(1u, hits.size());
  EXPECT_EQ(kAbbreviation, hits[0].type);
  ASSERT_TRUE(display->Lookup("rx/", &hits));
  EXPECT_EQ(kNormalSpelling, hits[0].type);
  ASSERT_TRUE(display->Lookup("rzz", &hits));

  auto other = AuxiliaryDb::Open("aux_path_other");
  ASSERT_TRUE(other);
  EXPECT_FALSE(other->LookupDisplay("你", &code));
}

TEST_F(RimeAuxiliaryLookupTest, RebuildsOnlyTheChangedGroup) {
  const path direct_path{"aux_path_test.aux.bin"};
  const auto direct_time = std::filesystem::last_write_time(direct_path);
  AuxiliaryDb other(path{"aux_path_other.aux.bin"});
  ASSERT_TRUE(other.Load());
  const auto original_checksum = other.checksum();
  other.Close();

  Config schema;
  ASSERT_TRUE(schema.LoadFromFile(path{"aux_path_test.schema.yaml"}));
  auto rules = schema.GetList("build/auxiliary/algebra/aux_path_other");
  ASSERT_TRUE(rules);
  ASSERT_TRUE(rules->Append(New<ConfigValue>("aux|^([a-z]{2,})$|$1|")));
  ASSERT_TRUE(schema.SaveToFile(path{"aux_path_test.schema.yaml"}));
  ASSERT_TRUE(
      DictCompiler::CompileAuxiliary(path{"aux_path_test.schema.yaml"}));
  EXPECT_EQ(direct_time, std::filesystem::last_write_time(direct_path));
  ASSERT_TRUE(other.Load());
  EXPECT_NE(original_checksum, other.checksum());
  vector<AuxiliaryCharacter> characters;
  ASSERT_TRUE(other.Lookup("rx", &characters));
  ASSERT_EQ(1u, characters.size());
  EXPECT_EQ(kNormalSpelling, characters.front().type);
  ASSERT_TRUE(database_->Lookup("rx", &characters));
  ASSERT_EQ(1u, characters.size());
  EXPECT_EQ(kAbbreviation, characters.front().type);
}

TEST_F(RimeAuxiliaryLookupTest, FlatRulesCompileAsAutoGroup) {
  Config schema;
  ASSERT_TRUE(schema.LoadFromFile(path{"aux_path_test.schema.yaml"}));
  auto rules = schema.GetList("build/auxiliary/algebra/aux_path_other");
  ASSERT_TRUE(rules);
  ASSERT_TRUE(schema.SetItem("build/auxiliary/algebra", rules));
  ASSERT_TRUE(schema.SaveToFile(path{"aux_path_test.schema.yaml"}));
  ASSERT_TRUE(
      DictCompiler::CompileAuxiliary(path{"aux_path_test.schema.yaml"}));
  AuxiliaryDb database(path{"auto.aux.bin"});
  ASSERT_TRUE(database.Load());
  vector<AuxiliaryCharacter> characters;
  EXPECT_TRUE(database.Lookup("rx/", &characters));
  EXPECT_FALSE(database.Lookup("rx", &characters));
}

TEST_F(RimeAuxiliaryLookupTest, NoRulesDoNotReadAuxiliarySource) {
  Config schema;
  ASSERT_TRUE(
      schema.SetString("build/auxiliary/dictionary", "nonexistent_aux_source"));
  ASSERT_TRUE(schema.SaveToFile(path{"aux_path_test.schema.yaml"}));
  EXPECT_TRUE(
      DictCompiler::CompileAuxiliary(path{"aux_path_test.schema.yaml"}));
}

TEST_F(RimeAuxiliaryLookupTest, KeepsOrdinaryPrefixCandidates) {
  const auto entries = Lookup("nihk");
  EXPECT_LT(Position(entries, "你好"), entries.size());
  EXPECT_LT(Position(entries, "拟好"), entries.size());
  EXPECT_LT(Position(entries, "泥蒿"), entries.size());
  EXPECT_LT(Position(entries, "你"), entries.size());
  EXPECT_LT(Position(entries, "泥"), entries.size());
}

TEST_F(RimeAuxiliaryLookupTest, AuxiliaryFiltersOnlyItsOwnPath) {
  const auto entries = Lookup("niz");
  EXPECT_LT(Position(entries, "逆"), entries.size());
  EXPECT_EQ(Position(entries, "你"), entries.size());
  EXPECT_EQ(Position(entries, "泥"), entries.size());
  const auto phrase = Lookup("nizln");
  EXPECT_LT(Position(phrase, "逆鳞"), phrase.size());
  EXPECT_EQ(Position(phrase, "你𤆄"), phrase.size());
}

TEST_F(RimeAuxiliaryLookupTest, KeepsOrdinaryPhraseAndAuxiliaryContinuation) {
  const auto short_phrase = Lookup("nizl");
  EXPECT_LT(Position(short_phrase, "你在"), short_phrase.size());
  const auto entries = Lookup("nizlnali");
  EXPECT_LT(Position(entries, "你在哪里"), entries.size());
}

TEST_F(RimeAuxiliaryLookupTest, UnrelatedCharactersDoNotCreateAuxiliaryEdges) {
  auto dict = OpenDictionary("pinyin");
  AuxiliaryMatcher matcher(dict.get(), database_.get());
  vector<AuxGraphMatch> matches;
  matcher.Lookup("haoz", &matches);
  EXPECT_TRUE(matches.empty());
  matcher.Lookup("nixy", &matches);
  ASSERT_EQ(2u, matches.size());
  EXPECT_EQ(kNormalSpelling, matches[0].type);
  EXPECT_EQ(kAbbreviation, matches[1].type);
  EXPECT_LT(matches[1].credibility, 0.0);
}

TEST_F(RimeAuxiliaryLookupTest,
       SuffixUsesCompiledCodesWithoutRetransformation) {
  auto dict = OpenDictionary("pinyin");
  AuxiliaryMatcher matcher(dict.get(), database_.get());
  EXPECT_TRUE(matcher.Match("你", "rx"));
  EXPECT_TRUE(matcher.Match("你在", "rx"));
  EXPECT_TRUE(matcher.Match("你", "rx/"));
  EXPECT_FALSE(matcher.Match("你", "rz"));
}

TEST_F(RimeAuxiliaryLookupTest, PinyinUsesMatchedSpellingLength) {
  const auto ordinary = Lookup("nizai", "pinyin");
  EXPECT_LT(Position(ordinary, "你在"), ordinary.size());
  const auto auxiliary = Lookup("nirzai", "pinyin");
  EXPECT_LT(Position(auxiliary, "你在"), auxiliary.size());
  EXPECT_EQ(Position(auxiliary, "拟好"), auxiliary.size());
}

TEST_F(RimeAuxiliaryLookupTest, AuxiliaryCodeHasNoFixedLength) {
  const auto entries = Lookup("niabcdef/zai", "pinyin");
  EXPECT_LT(Position(entries, "你在"), entries.size());
  EXPECT_EQ(Position(entries, "拟在哪里"), entries.size());
  const auto exact = Lookup("aiwy/", "pinyin");
  ASSERT_FALSE(exact.empty());
  EXPECT_EQ("哀", exact.front()->text);
}

TEST_F(RimeAuxiliaryLookupTest, AbbreviationKeepsPenaltyAndCharacterOrdering) {
  const auto entries = Lookup("zgjw");
  ASSERT_LT(Position(entries, "增加"), entries.size());
  ASSERT_LT(Position(entries, "增价"), entries.size());
  ASSERT_LT(Position(entries, "锃"), entries.size());
  EXPECT_LT(Position(entries, "增加"), Position(entries, "锃"));
  EXPECT_LT(Position(entries, "增价"), Position(entries, "锃"));
  EXPECT_TRUE(entries[Position(entries, "锃")]->abbreviated_auxiliary);
}

TEST_F(RimeAuxiliaryLookupTest,
       SearchOrderIsIndependentOfAuxiliaryDisplayOrder) {
  auto dict = OpenDictionary("pinyin");
  AuxiliaryMatcher matcher(dict.get(), database_.get());
  Syllabifier syllabifier(" '", false);
  syllabifier.SetAuxiliaryLookup(
      [&matcher](const string& code, vector<AuxGraphMatch>* matches) {
        matcher.Lookup(code, matches);
      });
  SyllableGraph graph;
  syllabifier.BuildSyllableGraph("abcdde", *dict->prism(), &graph);
  auto result = dict->Lookup(graph, 0);
  ASSERT_TRUE(result);
  auto bucket = result->find(4);
  ASSERT_NE(result->end(), bucket);
  auto& search = bucket->second;
  ASSERT_TRUE(search.Peek());
  EXPECT_EQ("甲乙", search.Peek()->text);
  EXPECT_FALSE(search.Peek()->abbreviated_auxiliary);

  auto display = search.Fork();
  display.ApplyAuxiliaryOrdering();
  ASSERT_TRUE(display.Peek());
  EXPECT_EQ("丙乙", display.Peek()->text);
  EXPECT_TRUE(display.Peek()->abbreviated_auxiliary);
  EXPECT_LT(display.Peek()->weight, search.Peek()->weight);
  EXPECT_EQ("甲乙", search.Peek()->text);

  auto filtered = search.Fork();
  filtered.AddFilter([](an<DictEntry> entry) { return entry->text != "丙乙"; });
  filtered.ApplyAuxiliaryOrdering();
  ASSERT_TRUE(filtered.Peek());
  EXPECT_EQ("甲乙", filtered.Peek()->text);
  EXPECT_EQ(1u, filtered.entry_count());
  EXPECT_FALSE(filtered.Next());
}

TEST_F(RimeAuxiliaryLookupTest, SameSpellingKeepsSeparateAuxiliaryMetadata) {
  const auto entries = Lookup("nixy", "pinyin");
  ASSERT_LT(Position(entries, "你"), entries.size());
  ASSERT_LT(Position(entries, "拟"), entries.size());
  const auto weak = entries[Position(entries, "你")];
  const auto normal = entries[Position(entries, "拟")];
  EXPECT_TRUE(weak->abbreviated_auxiliary);
  EXPECT_FALSE(normal->abbreviated_auxiliary);
  EXPECT_LT(weak->weight, normal->weight);
}

TEST_F(RimeAuxiliaryLookupTest, AuxiliaryWorksInLongEntryTail) {
  const auto entries = Lookup("nizlnaliz", "zrm", true, false);
  EXPECT_LT(Position(entries, "你在哪里"), entries.size());
  const auto first_aux = Lookup("nirhkzlnali", "zrm", true, false);
  EXPECT_LT(Position(first_aux, "你好在哪里"), first_aux.size());
  EXPECT_EQ(Position(first_aux, "拟好在哪里"), first_aux.size());
}

TEST_F(RimeAuxiliaryLookupTest, ExactPhrasePrecedesLongWordPrediction) {
  const auto entries = Lookup("nirhkzlna", "zrm", true, false, true);
  const auto exact = Position(entries, "你好在哪");
  const auto prediction = Position(entries, "你好在哪里");
  ASSERT_LT(exact, entries.size());
  ASSERT_LT(prediction, entries.size());
  EXPECT_LT(exact, prediction);
  EXPECT_TRUE(entries[exact]->IsExactMatch());
  EXPECT_TRUE(entries[prediction]->IsPredictiveMatch());
  EXPECT_EQ(Position(entries, "拟好在哪里"), entries.size());
}

TEST_F(RimeAuxiliaryLookupTest, WithoutAuxiliaryKeepsNativeResults) {
  const auto plain = Lookup("nihk", "zrm", false);
  const auto enabled = Lookup("nihk");
  ASSERT_EQ(plain.size(), enabled.size());
  for (size_t i = 0; i < plain.size(); ++i) {
    EXPECT_EQ(plain[i]->text, enabled[i]->text);
    EXPECT_EQ(plain[i]->code, enabled[i]->code);
    EXPECT_DOUBLE_EQ(plain[i]->weight, enabled[i]->weight);
    EXPECT_DOUBLE_EQ(plain[i]->quality_len, enabled[i]->quality_len);
  }
  for (const string input : {"nihk", "nizl", "nizlnali", "zgjw"}) {
    SCOPED_TRACE(input);
    const auto native = Lookup(input, "zrm", false);
    const auto auxiliary = Lookup(input);
    for (const auto& entry : native)
      EXPECT_LT(Position(auxiliary, entry->text), auxiliary.size());
  }
}

TEST_F(RimeAuxiliaryLookupTest, UserDictionaryFiltersTheChosenAuxiliaryPath) {
  auto dict = OpenDictionary("zrm");
  for (bool cached : {false, true}) {
    SCOPED_TRACE(cached ? "cached LevelDB" : "TextDB");
    an<Db> db;
    if (cached)
      db = New<UserDbWrapper<LevelDb>>(path{"aux_path_cache.userdb"},
                                       "aux_path_user");
    else
      db = New<UserDbWrapper<TextDb>>(path{"aux_path_user.txt"},
                                      "aux_path_user");
    ASSERT_TRUE(db->Open());
    ASSERT_TRUE(db->Update("ni lin \t逆鳞", "1,1,1,0,0"));
    ASSERT_TRUE(db->Update("ni lin \t你𤆄", "1,1,1,0,0"));
    ASSERT_TRUE(db->Update("ni \t你", "1,1,1,0,0"));
    UserDictionary user("aux_path_user", db);
    user.Attach(dict->primary_table(), dict->prism());
    ASSERT_TRUE(user.Load());
    user.SetUserDictMode("auto");
    AuxiliaryMatcher matcher(dict.get(), database_.get());
    Syllabifier syllabifier;
    syllabifier.SetAuxiliaryLookup(
        [&matcher](const string& code, vector<AuxGraphMatch>* matches) {
          matcher.Lookup(code, matches);
        });
    SyllableGraph graph;
    ASSERT_EQ(5,
              syllabifier.BuildSyllableGraph("nizln", *dict->prism(), &graph));
    auto result = user.Lookup(graph, 0);
    ASSERT_TRUE(result);
    auto full = result->find(5);
    ASSERT_NE(result->end(), full);
    ASSERT_FALSE(full->second.exhausted());
    EXPECT_EQ("逆鳞", full->second.Peek()->text);
    EXPECT_FALSE(full->second.Next());
    SyllableGraph ordinary_graph;
    ASSERT_EQ(4, syllabifier.BuildSyllableGraph("nihk", *dict->prism(),
                                                &ordinary_graph));
    auto ordinary_result = user.Lookup(ordinary_graph, 0);
    ASSERT_TRUE(ordinary_result);
    auto prefix = ordinary_result->find(2);
    ASSERT_NE(ordinary_result->end(), prefix);
    ASSERT_FALSE(prefix->second.exhausted());
    EXPECT_EQ("你", prefix->second.Peek()->text);
    ASSERT_TRUE(db->Close());
  }
}

TEST_F(RimeAuxiliaryLookupTest, BuildAllNineColumnAuxiliaryGroups) {
  database_.reset();
  std::remove("aux_path_test.aux.bin");
  std::remove("aux_path_other.aux.bin");
  std::ofstream("aux_path_test_aux.yaml") << "你\trx,ab,cd,ef,gh,ij,kl,mn,op\n";
  Config schema;
  ASSERT_TRUE(schema.LoadFromFile(path{"aux_path_test.schema.yaml"}));
  Config selection;
  std::istringstream data(R"YAML(input_types: [{option: pinyin}]
lookup:
  dictionary: aux_path_test
  aux: aux_path_test
algebra:
  aux_path_test:
    - "aux|^([a-z])([a-z])(?:,[^,]*){8}$|$1|"
    - "aux|^([a-z])([a-z])(?:,[^,]*){8}$|$1$2|abbrev"
    - "aux|^([a-z])([a-z])(?:,[^,]*){8}$|$1$2/|"
  aux_path_other:
    - "aux|^[^,]*,([a-z])([a-z])(?:,[^,]*){7}$|$1$2|"
)YAML");
  ASSERT_TRUE(selection.LoadFromStream(data));
  schema.SetItem("input_types", selection.GetItem("input_types"));
  schema.SetItem("lookup", selection.GetItem("lookup"));
  schema.SetItem("build/auxiliary/algebra", selection.GetItem("algebra"));
  ASSERT_TRUE(schema.SaveToFile(path{"aux_path_test.schema.yaml"}));
  ASSERT_TRUE(
      DictCompiler::CompileAuxiliary(path{"aux_path_test.schema.yaml"}));
  EXPECT_TRUE(std::filesystem::exists("aux_path_other.aux.bin"));
  AuxiliaryDb database(path{"aux_path_test.aux.bin"});
  ASSERT_TRUE(database.Load());
  vector<AuxiliaryCharacter> characters;
  ASSERT_TRUE(database.Lookup("r", &characters));
  ASSERT_EQ(1u, characters.size());
  EXPECT_EQ("你", characters.front().text);
  EXPECT_EQ(kNormalSpelling, characters.front().type);
  characters.clear();
  ASSERT_TRUE(database.Lookup("rx", &characters));
  EXPECT_EQ(kAbbreviation, characters.front().type);
  characters.clear();
  ASSERT_TRUE(database.Lookup("rx/", &characters));
  EXPECT_EQ(kNormalSpelling, characters.front().type);
  vector<AuxiliaryCode> codes;
  ASSERT_TRUE(database.ReverseLookup("你", &codes));
  EXPECT_EQ(3u, codes.size());
}

TEST_F(RimeAuxiliaryLookupTest, AuxOptionsDoNotRestrictBuildGroups) {
  database_.reset();
  std::remove("aux_path_test.aux.bin");
  std::remove("aux_path_other.aux.bin");
  Config schema;
  ASSERT_TRUE(schema.LoadFromFile(path{"aux_path_test.schema.yaml"}));
  Config selection;
  std::istringstream data(R"YAML(aux_types: [{option: aux_path_other}]
translator:
  dictionary: aux_path_test
  aux: aux_types
)YAML");
  ASSERT_TRUE(selection.LoadFromStream(data));
  ASSERT_TRUE(schema.SetItem("aux_types", selection.GetItem("aux_types")));
  ASSERT_TRUE(schema.SetItem("translator", selection.GetItem("translator")));
  ASSERT_TRUE(schema.SaveToFile(path{"aux_path_test.schema.yaml"}));
  ASSERT_TRUE(
      DictCompiler::CompileAuxiliary(path{"aux_path_test.schema.yaml"}));
  EXPECT_TRUE(std::filesystem::exists("aux_path_test.aux.bin"));
  EXPECT_TRUE(std::filesystem::exists("aux_path_other.aux.bin"));
}

TEST_F(RimeAuxiliaryLookupTest, RejectsMissingAuxiliaryDefinition) {
  Config schema;
  ASSERT_TRUE(schema.LoadFromFile(path{"aux_path_test.schema.yaml"}));
  std::istringstream settings(R"YAML(aux_types: [{option: missing}]
translator:
  dictionary: aux_path_test
  aux: aux_types
)YAML");
  Config input;
  ASSERT_TRUE(input.LoadFromStream(settings));
  ASSERT_TRUE(schema.SetItem("aux_types", input.GetItem("aux_types")));
  ASSERT_TRUE(schema.SetItem("translator", input.GetItem("translator")));
  ASSERT_TRUE(schema.SaveToFile(path{"aux_path_test.schema.yaml"}));
  EXPECT_FALSE(
      DictCompiler::CompileAuxiliary(path{"aux_path_test.schema.yaml"}));
}

TEST_F(RimeAuxiliaryLookupTest, SwitchesPreloadedAuxiliaryGroups) {
  database_.reset();
  auto config = std::make_unique<Config>();
  ASSERT_TRUE(config->LoadFromFile(path{"aux_path_test.schema.yaml"}));
  std::istringstream settings(R"YAML(aux_types:
  - {option: aux_path_test}
  - {option: aux_path_other}
engine:
  segmentors: [auxiliary_suffix_segmentor, abc_segmentor, fallback_segmentor]
  translators: [script_translator]
speller:
  alphabet: "abcdefghijklmnopqrstuvwxyz:"
  initials: abcdefghijklmnopqrstuvwxyz
auxiliary_suffix_segmentor:
  trigger: "::"
translator:
  dictionary: aux_path_test
  prism: zrm
  user_dict_mode: off
  aux: aux_types
  enable_completion: false
rules:
  - "aux|^rx$|z|"
)YAML");
  Config input;
  ASSERT_TRUE(input.LoadFromStream(settings));
  const vector<string> keys = {"aux_types", "engine", "speller",
                               "auxiliary_suffix_segmentor", "translator"};
  for (const auto& key : keys)
    ASSERT_TRUE(config->SetItem(key, input.GetItem(key)));
  ASSERT_TRUE(config->SetItem("build/auxiliary/algebra/aux_path_other",
                              input.GetItem("rules")));
  ASSERT_TRUE(config->SaveToFile(path{"aux_path_test.schema.yaml"}));
  ASSERT_TRUE(
      DictCompiler::CompileAuxiliary(path{"aux_path_test.schema.yaml"}));
  the<Engine> engine(Engine::Create());
  engine->ApplySchema(new Schema("aux_group_switch_test", config.release()));
  auto* context = engine->context();
  ASSERT_TRUE(engine->SelectSetting("aux_types", {"aux_path_test"}));
  context->set_input("ni::z");
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("逆", context->GetSelectedCandidate()->text());
  ASSERT_TRUE(engine->SelectSetting("aux_types", {"aux_path_other"}));
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("你", context->GetSelectedCandidate()->text());
  ASSERT_TRUE(engine->SelectSetting("aux_types", {"aux_path_test"}));
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("逆", context->GetSelectedCandidate()->text());
}

TEST_F(RimeAuxiliaryLookupTest,
       CopiesAndSkipPreserveAuxiliaryPathsAndOrdering) {
  auto dictionary = OpenDictionary("zrm");
  AuxiliaryMatcher matcher(dictionary.get(), database_.get());
  auto drain = [](DictEntryIterator& iterator) {
    vector<string> entries;
    while (!iterator.exhausted()) {
      auto entry = iterator.Peek();
      entries.push_back(entry->text +
                        (entry->abbreviated_auxiliary ? ":aux" : ""));
      iterator.Next();
    }
    return entries;
  };
  for (const string input : {"nixy", "nizln", "nizlnaliz", "zgjw"}) {
    SCOPED_TRACE(input);
    Syllabifier syllabifier(" '", false);
    syllabifier.SetAuxiliaryLookup(
        [&matcher](const string& code, vector<AuxGraphMatch>* matches) {
          matcher.Lookup(code, matches);
        });
    SyllableGraph graph;
    syllabifier.BuildSyllableGraph(input, *dictionary->prism(), &graph);
    auto collector = dictionary->Lookup(graph, 0);
    ASSERT_TRUE(collector);
    auto bucket = collector->find(input.size());
    ASSERT_NE(collector->end(), bucket);
    auto& original = bucket->second;
    original.ApplyAuxiliaryOrdering();
    DictEntryIterator copy(original);
    DictEntryIterator assigned;
    assigned = original;
    auto fork = original.Fork();
    const auto expected = drain(copy);
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(expected, drain(assigned));
    EXPECT_EQ(expected, drain(fork));
    ASSERT_TRUE(original.Peek());
    ASSERT_TRUE(original.Skip(1));
    EXPECT_EQ(vector<string>(expected.begin() + 1, expected.end()),
              drain(original));
  }
}

TEST_F(RimeAuxiliaryLookupTest,
       AbbreviatedAuxiliaryPhraseDoesNotSuppressSentenceMaking) {
  for (const string type : {"zrm", "pinyin"}) {
    for (auto [max_sentences, max_homophones] :
         {std::pair{1, 1}, std::pair{3, 8}}) {
      SCOPED_TRACE(::testing::Message()
                   << "prism=" << type << " max_sentences=" << max_sentences
                   << " max_homophones=" << max_homophones);
      auto config = std::make_unique<Config>();
      ASSERT_TRUE(config->LoadFromFile(path{"aux_path_test.schema.yaml"}));
      std::istringstream settings(R"YAML(engine:
  segmentors: [abc_segmentor, fallback_segmentor]
  translators: [script_translator]
speller:
  alphabet: abcdefghijklmnopqrstuvwxyz/
translator:
  dictionary: aux_path_test
  aux: aux_path_test
  direct_aux_mode: char
  user_dict_mode: off
  enable_completion: false
)YAML");
      Config input;
      ASSERT_TRUE(input.LoadFromStream(settings));
      for (const string key : {"engine", "speller", "translator"})
        ASSERT_TRUE(config->SetItem(key, input.GetItem(key)));
      ASSERT_TRUE(config->SetString("translator/prism", type));
      ASSERT_TRUE(config->SetInt("translator/max_sentences", max_sentences));
      ASSERT_TRUE(config->SetInt("translator/max_homophones", max_homophones));
      the<Engine> engine(Engine::Create());
      engine->ApplySchema(new Schema("aux_sentence_test", config.release()));
      auto* context = engine->context();
      auto query = [&](const string& raw, const string& expected,
                       bool sentence) {
        context->Clear();
        context->set_input(raw);
        auto candidate = context->GetSelectedCandidate();
        ASSERT_TRUE(candidate);
        EXPECT_EQ(expected, candidate->text());
        EXPECT_EQ(raw.size(), candidate->end());
        EXPECT_EQ(sentence, candidate->type() == "sentence");
      };
      const string base = type == "zrm" ? "zv" : "zui";
      const string tail = type == "zrm" ? "ky" : "kuai";
      const string phrase = base + tail;
      // A complete phrase reached through weak AUX must not hide the ordinary
      // path that needs sentence-making to produce its final word.
      query(phrase + "de", "最快的", true);
      query(phrase, "最快", false);
      // Explicit non-abbreviated AUX remains a reliable full phrase match.
      query(phrase + "/de", "嘴的", false);
      // A weak AUX head does not hide the ordinary exact word behind it.
      query("abcd", "丙乙", false);
      // Display priority must not evict the ordinary path at max_homophones=1.
      query("abcdde", "甲乙的", true);
      if (type == "zrm")
        query("zgjwde", "增加的", true);
    }
  }
}

TEST_F(RimeAuxiliaryLookupTest, UserAuxiliaryPriorityKeepsOrdinaryMatches) {
  for (bool complete_phrase : {false, true}) {
    SCOPED_TRACE(complete_phrase ? "complete user phrase" : "user prefix");
    const string user_name =
        complete_phrase ? "aux_path_sentence" : "aux_path_prefix";
    {
      UserDbWrapper<LevelDb> db(path{user_name + ".userdb"}, user_name);
      ASSERT_TRUE(db.Open());
      if (complete_phrase) {
        ASSERT_TRUE(db.Update("a de \t丙的", "1,1,1,1,1"));
        ASSERT_TRUE(db.Update("ab de \t甲的", "1,1,1,0,0"));
      } else {
        ASSERT_TRUE(db.Update("a \t丙", "1,1,1,1,1"));
      }
      ASSERT_TRUE(db.Close());
    }
    auto config = std::make_unique<Config>();
    ASSERT_TRUE(config->LoadFromFile(path{"aux_path_test.schema.yaml"}));
    std::istringstream settings(R"YAML(engine:
  segmentors: [abc_segmentor, fallback_segmentor]
  translators: [script_translator]
translator:
  dictionary: aux_path_test
  prism: pinyin
  aux: aux_path_test
  direct_aux_mode: char
  user_dict_mode: auto
  enable_completion: false
)YAML");
    Config input;
    ASSERT_TRUE(input.LoadFromStream(settings));
    for (const string key : {"engine", "translator"})
      ASSERT_TRUE(config->SetItem(key, input.GetItem(key)));
    ASSERT_TRUE(config->SetString("translator/user_dict", user_name));
    the<Engine> engine(Engine::Create());
    engine->ApplySchema(new Schema("aux_user_sentence_test", config.release()));
    engine->context()->set_input("abde");
    auto candidate = engine->context()->GetSelectedCandidate();
    ASSERT_TRUE(candidate);
    EXPECT_EQ(complete_phrase ? "丙的" : "甲的", candidate->text());
    EXPECT_EQ(complete_phrase ? "user_phrase" : "sentence", candidate->type());
    EXPECT_EQ(4u, candidate->end());
  }
}

TEST_F(RimeAuxiliaryLookupTest, EngineControlsDirectAndSuffixIndependently) {
  for (const string mode : {"", "off", "char", "word", "invalid"}) {
    for (bool suffix : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "mode=" << mode << " suffix=" << suffix);
      auto config = std::make_unique<Config>();
      ASSERT_TRUE(config->LoadFromFile(path{"aux_path_test.schema.yaml"}));
      std::istringstream settings(R"YAML(engine:
  segmentors:
    - {name: auxiliary_suffix_segmentor, when: suffix}
    - abc_segmentor
    - fallback_segmentor
  translators:
    - script_translator
    - table_translator@plain
    - echo_translator
speller:
  alphabet: "abcdefghijklmnopqrstuvwxyz:"
  initials: abcdefghijklmnopqrstuvwxyz
auxiliary_suffix_segmentor:
  trigger: "::"
translator:
  dictionary: aux_path_test
  prism: zrm
  user_dict_mode: off
  aux: aux_path_test
  enable_completion: false
plain:
  dictionary: aux_path_test
  prism: zrm
  user_dict_mode: off
  enable_completion: false
)YAML");
      Config input;
      ASSERT_TRUE(input.LoadFromStream(settings));
      for (const string key :
           {"engine", "speller", "auxiliary_suffix_segmentor", "translator",
            "plain"})
        ASSERT_TRUE(config->SetItem(key, input.GetItem(key)));
      if (!mode.empty()) {
        ASSERT_TRUE(config->SetString("translator/direct_aux_mode", mode));
      }
      the<Engine> engine(Engine::Create());
      engine->ApplySchema(new Schema("aux_controls_test", config.release()));
      auto* context = engine->context();
      auto query = [&](const string& raw) {
        context->Clear();
        context->set_input(raw);
        vector<pair<string, size_t>> entries;
        if (context->composition().empty())
          return entries;
        auto menu = context->composition().back().menu;
        if (!menu)
          return entries;
        const auto count = menu->Prepare(20);
        for (size_t i = 0; i < count; ++i) {
          auto candidate = menu->GetCandidateAt(i);
          if (candidate)
            entries.emplace_back(candidate->text(), candidate->end());
        }
        return entries;
      };

      context->set_option("suffix", suffix);
      const auto direct_hit = std::make_pair(string("逆"), size_t{3});
      const auto direct_candidates = query("niz");
      EXPECT_EQ(mode == "char",
                std::find(direct_candidates.begin(), direct_candidates.end(),
                          direct_hit) != direct_candidates.end());

      const auto suffix_candidates = query("ni::z");
      ASSERT_FALSE(context->composition().empty());
      EXPECT_EQ(suffix,
                context->composition().back().auxiliary_suffix.has_value());
      if (!suffix)
        continue;

      const vector<pair<string, size_t>> expected{{"逆", 2}};
      EXPECT_EQ(expected, suffix_candidates);
      context->set_caret_pos(4);  // the code after the caret is inactive
      auto before_code = context->GetSelectedCandidate();
      ASSERT_TRUE(before_code);
      EXPECT_EQ("你", before_code->text());
      context->set_caret_pos(5);
      auto with_code = context->GetSelectedCandidate();
      ASSERT_TRUE(with_code);
      EXPECT_EQ("逆", with_code->text());
      ASSERT_TRUE(context->PopInput());
      auto pending = context->GetSelectedCandidate();
      ASSERT_TRUE(pending);
      EXPECT_EQ("你", pending->text());
      ASSERT_TRUE(context->PopInput(2));
      EXPECT_FALSE(context->composition().back().auxiliary_suffix);

      EXPECT_TRUE(query("ni::qq").empty());
      EXPECT_EQ(expected, query("ni::z"));
      ASSERT_TRUE(context->ConfirmCurrentSelection());
      EXPECT_EQ(5u, context->composition().GetConfirmedPosition());
      EXPECT_FALSE(context->HasMenu());
      EXPECT_EQ("逆", context->GetCommitText());
      ASSERT_TRUE(context->ReopenPreviousSegment());
      EXPECT_EQ(2u, context->composition().back().end);
      ASSERT_TRUE(context->GetSelectedCandidate());
      EXPECT_EQ("逆", context->GetSelectedCandidate()->text());
      ASSERT_TRUE(context->ConfirmCurrentSelection());
      ASSERT_TRUE(context->PushInput("hk"));
      EXPECT_EQ("逆好", context->GetCommitText());
      EXPECT_TRUE(context->Commit());
      EXPECT_EQ(string::npos, context->commit_history().repr().find("::z"));
    }
  }
}

TEST_F(RimeAuxiliaryLookupTest, WordAuxiliaryFollowsExactSuffix) {
  std::ofstream("aux_path_test.dict.yaml", std::ios::app)
      << "戊\tab\t1000\n"
         "丁\tq\t100\n"
         "戊乙\tab cd\t100000\n"
         "甲乙丁\tab cd q\t100\n"
         "甲乙乙乙乙\tab cd cd cd cd\t100\n";
  std::ofstream("aux_path_test_aux.yaml", std::ios::app) << "甲\tqrst\n";
  database_->Close();
  const path schema_file{"aux_path_test.schema.yaml"};
  ASSERT_TRUE(DictCompiler::CompileAuxiliary(schema_file));
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_file));
  {
    UserDbWrapper<LevelDb> db(path{"aux_path_word.userdb"}, "aux_path_word");
    ASSERT_TRUE(db.Open());
    ASSERT_TRUE(db.Update("ab cd \t甲戊", "1,1,1,0,0"));
    ASSERT_TRUE(db.Close());
  }
  auto config = std::make_unique<Config>();
  ASSERT_TRUE(config->LoadFromFile(schema_file));
  std::istringstream settings(R"YAML(engine:
  segmentors: [abc_segmentor, fallback_segmentor]
  translators: [script_translator]
speller:
  alphabet: abcdefghijklmnopqrstuvwxyz/
translator:
  dictionary: aux_path_test
  prism: pinyin
  user_dict: aux_path_word
  user_dict_mode: auto
  aux: aux_path_test
  direct_aux_mode: word
)YAML");
  Config input;
  ASSERT_TRUE(input.LoadFromStream(settings));
  for (const string key : {"engine", "speller", "translator"})
    ASSERT_TRUE(config->SetItem(key, input.GetItem(key)));
  the<Engine> engine(Engine::Create());
  engine->ApplySchema(new Schema("word_aux_test", config.release()));
  auto* context = engine->context();
  auto find_full = [&](const string& text) -> an<Candidate> {
    if (context->composition().empty())
      return nullptr;
    const auto menu = context->composition().back().menu;
    if (!menu)
      return nullptr;
    const auto count = menu->Prepare(50);
    for (size_t i = 0; i < count; ++i) {
      auto candidate = menu->GetCandidateAt(i);
      if (candidate && candidate->text() == text &&
          candidate->end() == context->input().size())
        return candidate;
    }
    return nullptr;
  };

  context->set_input("abcdq");
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("甲乙丁", context->GetSelectedCandidate()->text());
  EXPECT_TRUE(find_full("甲乙"));
  auto user_phrase = find_full("甲戊");
  ASSERT_TRUE(user_phrase);
  EXPECT_EQ("user_phrase", user_phrase->type());
  EXPECT_FALSE(find_full("戊乙"));
  context->set_input("abqcd");
  EXPECT_FALSE(find_full("甲乙"));

  context->set_input("abq");
  EXPECT_FALSE(find_full("甲"));
  context->set_input("ababqrst");
  EXPECT_FALSE(find_full("甲甲"));
  context->set_input("abcdcdcdcdqrst");
  EXPECT_TRUE(find_full("甲乙乙乙乙"));
  context->set_input("abcdqrst/");
  EXPECT_TRUE(find_full("甲乙"));

  context->set_input("abcdqrst");
  auto match = find_full("甲乙");
  ASSERT_TRUE(match);
  EXPECT_EQ("ab cd qrst", match->preedit());
  auto phrase = As<Phrase>(Candidate::GetGenuineCandidate(match));
  ASSERT_TRUE(phrase);
  EXPECT_EQ((vector<size_t>{0, 2, 8}), phrase->spans().vertices());
  ASSERT_TRUE(context->PushInput("u"));
  EXPECT_FALSE(find_full("甲乙"));
  ASSERT_TRUE(context->PopInput());
  EXPECT_TRUE(find_full("甲乙"));
  ASSERT_TRUE(context->GetSelectedCandidate());
  ASSERT_TRUE(context->ConfirmCurrentSelection());
  EXPECT_EQ(8u, context->composition().GetConfirmedPosition());
}

TEST_F(RimeAuxiliaryLookupTest, WordAuxiliaryRespectsCompleteTailSpelling) {
  std::ofstream("aux_path_test.dict.yaml", std::ios::app)
      << "再\tzai\t100\n"
         "发\tfa\t100\n"
         "烦\tfan\t100\n"
         "佛\tfo\t100\n"
         "拟在\tni zai\t10\n"
         "你再发\tni zai fa\t100\n"
         "你再烦\tni zai fan\t100000000\n"
         "你在快\tni zai kuai\t100\n";
  std::ofstream("aux_path_test_aux.yaml", std::ios::app)
      << "拟\tfa\n拟\tfx\n拟\tfo\n";
  database_->Close();
  const path schema_file{"aux_path_test.schema.yaml"};
  std::ofstream(schema_file.c_str()) << R"YAML(build:
  dictionaries:
    aux_path_test:
      algebra:
        pinyin:
          - abbrev/^([a-z]).+$/$1/
          - abbrev/^fan$/fa/
        zrm:
          - xform/^zai$/zl/
          - xform/^fan$/fj/
          - xform/^kuai$/ky/
          - abbrev/^([a-z]).+$/$1/
          - abbrev/^fj$/fa/
  auxiliary:
    dictionary: aux_path_test_aux
    algebra:
      aux_path_test:
        - "aux|^([a-z])[a-z]*$|$1|"
        - "aux|^([a-z]{2,})$|$1|abbrev"
)YAML";
  ASSERT_TRUE(DictCompiler::CompileAuxiliary(schema_file));
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_file));

  for (const string type : {"pinyin", "zrm"}) {
    for (const string mode : {"", "off", "word", "invalid"}) {
      SCOPED_TRACE(::testing::Message() << type << " mode=" << mode);
      auto config = std::make_unique<Config>();
      ASSERT_TRUE(config->LoadFromFile(schema_file));
      std::istringstream settings(R"YAML(engine:
  segmentors: [abc_segmentor, fallback_segmentor]
  translators: [script_translator]
speller:
  alphabet: abcdefghijklmnopqrstuvwxyz/
translator:
  dictionary: aux_path_test
  user_dict_mode: off
  aux: aux_path_test
  enable_completion: false
)YAML");
      Config input;
      ASSERT_TRUE(input.LoadFromStream(settings));
      for (const string key : {"engine", "speller", "translator"})
        ASSERT_TRUE(config->SetItem(key, input.GetItem(key)));
      ASSERT_TRUE(config->SetString("translator/prism", type));
      if (!mode.empty()) {
        ASSERT_TRUE(config->SetString("translator/direct_aux_mode", mode));
      }
      the<Engine> engine(Engine::Create());
      engine->ApplySchema(new Schema("word_aux_tail_test", config.release()));
      auto* context = engine->context();
      auto query = [&](const string& raw) {
        context->Clear();
        context->set_input(raw);
        vector<string> words;
        if (context->composition().empty())
          return words;
        const auto menu = context->composition().back().menu;
        if (!menu)
          return words;
        const auto count = menu->Prepare(50);
        for (size_t i = 0; i < count; ++i) {
          auto candidate = menu->GetCandidateAt(i);
          if (candidate && candidate->end() == raw.size())
            words.push_back(candidate->text());
        }
        return words;
      };
      const string base = type == "zrm" ? "nizl" : "nizai";
      const auto matched = query(base + "f");
      ASSERT_FALSE(matched.empty());
      EXPECT_EQ(mode == "word" ? "拟在" : "你再烦", matched.front());
      if (mode != "word")
        continue;
      EXPECT_EQ(1, std::count(matched.begin(), matched.end(), "你再烦"));
      EXPECT_EQ(1, std::count(matched.begin(), matched.end(), "拟在"));

      // A real complete last syllable wins even if the word AUX also matches.
      const auto complete = query(base + "fa");
      ASSERT_FALSE(complete.empty());
      EXPECT_EQ("你再发", complete.front());
      EXPECT_EQ(1, std::count(complete.begin(), complete.end(), "拟在"));

      // A valid complete spelling without a real phrase is not sufficient.
      const auto no_phrase = query(base + "fo");
      ASSERT_FALSE(no_phrase.empty());
      EXPECT_EQ("拟在", no_phrase.front());
      const auto unmatched = query(base + "k");
      ASSERT_FALSE(unmatched.empty());
      EXPECT_EQ("你在快", unmatched.front());

      // Earlier syllables can be abbreviations. A high-weight weak candidate
      // must not hide a complete final syllable behind it.
      const auto short_weak = query("nzf");
      ASSERT_FALSE(short_weak.empty());
      EXPECT_EQ("拟在", short_weak.front());
      const auto short_complete = query("nzfa");
      ASSERT_FALSE(short_complete.empty());
      EXPECT_EQ("你再发", short_complete.front());
      const auto& words = short_complete;
      EXPECT_EQ(1, std::count(words.begin(), words.end(), "你再烦"));
    }
  }
}

TEST_F(RimeAuxiliaryLookupTest, DirectAuxModesAreExclusive) {
  std::ofstream("aux_path_test.dict.yaml", std::ios::app)
      << "扗\tzai\t1\n"
         "发\tfa\t100\n"
         "烦\tfan\t100\n"
         "拟在\tni zai\t10\n"
         "你再发\tni zai fa\t100\n"
         "你再烦\tni zai fan\t100\n";
  std::ofstream("aux_path_test_aux.yaml", std::ios::app) << "拟\tfr\n扗\tft\n";
  database_->Close();
  const path schema_file{"aux_path_test.schema.yaml"};
  std::ofstream(schema_file.c_str()) << R"YAML(build:
  dictionaries:
    aux_path_test:
      algebra:
        zrm:
          - xform/^zai$/zl/
          - xform/^fan$/fj/
  auxiliary:
    dictionary: aux_path_test_aux
    algebra:
      aux_path_test:
        - "aux|^([a-z])[a-z]*$|$1|"
        - "aux|^([a-z]{2,})$|$1|abbrev"
)YAML";
  ASSERT_TRUE(DictCompiler::CompileAuxiliary(schema_file));
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_file));
  for (const string mode : {"off", "char", "word"}) {
    SCOPED_TRACE(mode);
    auto config = std::make_unique<Config>();
    ASSERT_TRUE(config->LoadFromFile(schema_file));
    std::istringstream settings(R"YAML(engine:
  segmentors: [abc_segmentor, fallback_segmentor]
  translators: [script_translator]
speller:
  alphabet: abcdefghijklmnopqrstuvwxyz
translator:
  dictionary: aux_path_test
  prism: zrm
  aux: aux_path_test
  user_dict_mode: off
  enable_completion: true
)YAML");
    Config input;
    ASSERT_TRUE(input.LoadFromStream(settings));
    for (const string key : {"engine", "speller", "translator"})
      ASSERT_TRUE(config->SetItem(key, input.GetItem(key)));
    ASSERT_TRUE(config->SetString("translator/direct_aux_mode", mode));
    the<Engine> engine(Engine::Create());
    engine->ApplySchema(new Schema("direct_aux_mode_test", config.release()));
    auto* context = engine->context();
    context->set_input("nizlf");
    ASSERT_FALSE(context->composition().empty());
    const auto menu = context->composition().back().menu;
    ASSERT_TRUE(menu);
    vector<string> words;
    const auto count = menu->Prepare(50);
    for (size_t i = 0; i < count; ++i) {
      auto candidate = menu->GetCandidateAt(i);
      if (candidate && candidate->end() == 5)
        words.push_back(candidate->text());
    }
    ASSERT_FALSE(words.empty());
    EXPECT_EQ(mode == "word",
              std::count(words.begin(), words.end(), "拟在") == 1);
    EXPECT_EQ(mode == "char",
              std::count(words.begin(), words.end(), "你扗") == 1);
    EXPECT_EQ(mode != "char",
              std::count(words.begin(), words.end(), "你再发") == 1);
    EXPECT_EQ(mode != "char",
              std::count(words.begin(), words.end(), "你再烦") == 1);
    if (mode == "word") {
      EXPECT_EQ("拟在", words.front());
    }
  }
}
