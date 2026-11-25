//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-05-17 Zou xu <zouivex@gmail.com>
//
#include <algorithm>
#include <fstream>
#include <string_view>
#include <gtest/gtest.h>
#include <rime/dict/prism.h>
#include <rime/algo/algebra.h>

using namespace rime;

class RimePrismTest : public ::testing::Test {
 protected:
  RimePrismTest() {}

  virtual void SetUp() {
    prism_.reset(new Prism(path{"prism_test.bin"}));
    prism_->Remove();
    set<string> keyset;
    keyset.insert("google");   // 4
    keyset.insert("good");     // 2
    keyset.insert("goodbye");  // 3
    keyset.insert("microsoft");
    keyset.insert("macrosoft");
    keyset.insert("adobe");  // 0 == id
    keyset.insert("yahoo");
    keyset.insert("baidu");  // 1

    prism_->Build(keyset);
  }

  virtual void TearDown() {}

  the<Prism> prism_;
};

TEST_F(RimePrismTest, SaveAndLoad) {
  EXPECT_TRUE(prism_->Save());
  Prism test(prism_->file_path());
  EXPECT_TRUE(test.Load());

  EXPECT_EQ(prism_->array_size(), test.array_size());
}

TEST_F(RimePrismTest, Metadata) {
  EXPECT_EQ(prism_->max_key_length(), 9);  // "macrosoft"
}

TEST_F(RimePrismTest, HasKey) {
  EXPECT_TRUE(prism_->HasKey("google"));
  EXPECT_FALSE(prism_->HasKey("googlesoft"));

  EXPECT_TRUE(prism_->HasKey("microsoft"));
  EXPECT_FALSE(prism_->HasKey("peoplesoft"));
}

TEST_F(RimePrismTest, GetValue) {
  int value = -1;
  EXPECT_TRUE(prism_->GetValue("adobe", &value));
  EXPECT_EQ(value, 0);
  value = -1;
  EXPECT_TRUE(prism_->GetValue("baidu", &value));
  EXPECT_EQ(value, 1);
}

TEST_F(RimePrismTest, CommonPrefixMatch) {
  vector<Prism::Match> result;

  prism_->CommonPrefixSearch("goodbye", &result);
  // result is good and goodbye.
  ASSERT_EQ(result.size(), 2);
  EXPECT_EQ(result[0].value, 2);   // good
  EXPECT_EQ(result[0].length, 4);  // good
  EXPECT_EQ(result[1].value, 3);   // goodbye
  EXPECT_EQ(result[1].length, 7);  // goodbye
}
TEST_F(RimePrismTest, ExpandSearch) {
  vector<Prism::Match> result;

  prism_->ExpandSearch("goo", &result, 10);
  // result is good, google and goodbye (ordered by length asc).
  ASSERT_EQ(result.size(), 3);
  EXPECT_EQ(result[0].value, 2);   // good
  EXPECT_EQ(result[0].length, 4);  // good
  EXPECT_EQ(result[1].value, 4);   // google
  EXPECT_EQ(result[1].length, 6);  // google
  EXPECT_EQ(result[2].value, 3);   // goodbye
  EXPECT_EQ(result[2].length, 7);  // goodbye
}
TEST_F(RimePrismTest, ExpandSearchLimit) {
  vector<Prism::Match> result;
  prism_->ExpandSearch("goo", &result, 2);
  // result is good and google.

  ASSERT_EQ(result.size(), 2);
  EXPECT_EQ(result[0].value, 2);
  EXPECT_EQ(result[0].length, 4);
  EXPECT_EQ(result[1].value, 4);
  EXPECT_EQ(result[1].length, 6);
}

TEST_F(RimePrismTest, CommonPrefixSearchWithLength) {
  const string input("xxgoodbye");
  vector<Prism::Match> result;

  prism_->CommonPrefixSearch(std::string_view(input).substr(2, 4), &result);
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result[0].value, 2);
  EXPECT_EQ(result[0].length, 4);
}

TEST_F(RimePrismTest, ExpandSearchWithLength) {
  const string input("xxgoodbye");
  vector<Prism::Match> result;

  prism_->ExpandSearch(std::string_view(input).substr(2, 3), &result, 10);

  ASSERT_EQ(result.size(), 3);
  EXPECT_EQ(result[0].value, 2);
  EXPECT_EQ(result[0].length, 4);
  EXPECT_EQ(result[1].value, 4);
  EXPECT_EQ(result[1].length, 6);
  EXPECT_EQ(result[2].value, 3);
  EXPECT_EQ(result[2].length, 7);
}

static vector<string> ReadChoices(prism::CodeChoiceView choices) {
  vector<string> result;
  for (size_t i = 0; i < choices.size(); ++i)
    result.emplace_back(choices[i]);
  return result;
}

TEST_F(RimePrismTest, CodeChoicesUseCompiledSyllableSubsets) {
  Prism prism(path{"code_choices_test.prism.bin"});
  prism.Remove();
  const Syllabary syllables{"x1", "x2", "y1", "z1", "z2", "outside"};
  Script script;
  script["986"] = {Spelling("x1"), Spelling("x2"), Spelling("y1"),
                   Spelling("z1"), Spelling("z2")};
  script["alias"] = script["986"];
  script["xun"] = {Spelling("x1"), Spelling("x2")};
  script["xvn"] = script["xun"];
  script["Y"] = {Spelling("y1")};
  script["zuo"] = {Spelling("z1"), Spelling("z2")};
  script["zun"] = {Spelling("z1"), Spelling("outside")};
  script["xung"] = script["xun"];
  script["abbr"] = {Spelling("x1")};
  script["abbr"].front().properties.type = kAbbreviation;
  ASSERT_TRUE(prism.Build(syllables, &script));
  const vector<string> expected{"Y", "xun", "zuo"};
  EXPECT_EQ(expected, ReadChoices(prism.QueryCodeChoices("986")));
  ASSERT_TRUE(prism.Save());
  ASSERT_TRUE(prism.Load());
  EXPECT_TRUE(prism.has_code_choice_format());
  EXPECT_TRUE(prism.HasCodeChoices());
  EXPECT_EQ(expected, ReadChoices(prism.QueryCodeChoices("986")));
  EXPECT_EQ(expected, ReadChoices(prism.QueryCodeChoices("alias")));
  EXPECT_TRUE(prism.QueryCodeChoices("xun").empty());
  EXPECT_TRUE(prism.QueryCodeChoices("9864").empty());
  EXPECT_TRUE(prism.QueryCodeChoices(-1).empty());
  int id = -1;
  ASSERT_TRUE(prism.GetValue("986", &id));
  EXPECT_EQ(expected, ReadChoices(prism.QueryCodeChoices(id)));
  ASSERT_TRUE(prism.GetValue("zuo", &id));
  EXPECT_TRUE(prism.QueryCodeChoices(id).empty());

  // Legacy format is readable but must be rebuilt by the compiler.
  prism.Close();
  {
    std::fstream file(prism.file_path().c_str(),
                      std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file);
    file.write("Rime::Prism/6.0", 15);
  }
  ASSERT_TRUE(prism.Load());
  EXPECT_FALSE(prism.has_code_choice_format());
  EXPECT_FALSE(prism.HasCodeChoices());
  EXPECT_TRUE(prism.QueryCodeChoices("986").empty());
  prism.Remove();
}

TEST_F(RimePrismTest, NoCodeChoicesForIdentityOrEqualSets) {
  EXPECT_FALSE(prism_->HasCodeChoices());
  EXPECT_TRUE(prism_->QueryCodeChoices("good").empty());
  Prism prism(path{"equal_choices_test.prism.bin"});
  prism.Remove();
  const Syllabary syllables{"a", "b"};
  Script script;
  script["23"] = {Spelling("a"), Spelling("b")};
  script["ab"] = script["23"];
  ASSERT_TRUE(prism.Build(syllables, &script));
  ASSERT_TRUE(prism.Save());
  ASSERT_TRUE(prism.Load());
  EXPECT_TRUE(prism.has_code_choice_format());
  EXPECT_FALSE(prism.HasCodeChoices());
  EXPECT_TRUE(prism.QueryCodeChoices("23").empty());
  EXPECT_TRUE(prism.QueryCodeChoices("ab").empty());
  prism.Remove();
}
