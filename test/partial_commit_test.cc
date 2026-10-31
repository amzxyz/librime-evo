//
// Copyright RIME Developers
// Distributed under the BSD License
//

#include <gtest/gtest.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/gear/partial_commit.h>
#include <rime/gear/translator_commons.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/translation.h>

namespace rime {
namespace {

class FixedSyllabifier : public PhraseSyllabifier {
 public:
  Spans Syllabify(const Phrase*) override {
    Spans result;
    result.set_vertices({0, 2, 4, 6});
    return result;
  }
};

class PartialCommitTest : public ::testing::Test {
 protected:
  void SetUpProcessor(bool control_digits = false) {
    engine_.reset(Engine::Create());
    auto* config = new Config;
    config->SetBool("partial_commit/control_digits", control_digits);
    config->SetString("partial_commit/first_key", "[");
    config->SetString("partial_commit/last_key", "]");
    engine_->ApplySchema(new Schema("partial_commit_test", config));
    processor_.reset(new PartialCommit(Ticket(engine_.get(), "processor")));
  }

  void SetPhraseComposition() {
    auto entry = New<DictEntry>();
    entry->text = "你好世界";
    auto phrase = New<Phrase>(nullptr, "test", 0, 6, entry);
    phrase->set_syllabifier(New<FixedSyllabifier>());

    auto menu = New<Menu>();
    menu->AddTranslation(New<UniqueTranslation>(phrase));
    ASSERT_EQ(1u, menu->Prepare(1));

    engine_->context()->set_input("abcdef");
    Composition& composition = engine_->context()->composition();
    composition.Reset(1);
    composition[0] = Segment(0, 6);
    composition[0].status = Segment::kGuess;
    composition[0].menu = menu;
  }

  the<Engine> engine_;
  the<PartialCommit> processor_;
};

TEST_F(PartialCommitTest, CommitsFirstCharacter) {
  SetUpProcessor();
  engine_->context()->set_input("你好");

  EXPECT_EQ(kAccepted,
            processor_->ProcessKeyEvent(KeyEvent(XK_bracketleft, 0)));
  EXPECT_EQ("", engine_->context()->input());
  EXPECT_EQ("你", engine_->context()->commit_history().latest_text());
}

TEST_F(PartialCommitTest, CommitsLastCharacter) {
  SetUpProcessor();
  engine_->context()->set_input("你好");

  EXPECT_EQ(kAccepted,
            processor_->ProcessKeyEvent(KeyEvent(XK_bracketright, 0)));
  EXPECT_EQ("", engine_->context()->input());
  EXPECT_EQ("好", engine_->context()->commit_history().latest_text());
}

TEST_F(PartialCommitTest, IgnoresSingleCharacterInput) {
  SetUpProcessor();
  engine_->context()->set_input("你");

  EXPECT_EQ(kNoop, processor_->ProcessKeyEvent(KeyEvent(XK_bracketleft, 0)));
  EXPECT_EQ("你", engine_->context()->input());
}

TEST_F(PartialCommitTest, CommitsPrefixAndKeepsRemainingInput) {
  SetUpProcessor(true);
  SetPhraseComposition();

  EXPECT_EQ(kAccepted,
            processor_->ProcessKeyEvent(KeyEvent(XK_2, kControlMask)));
  EXPECT_EQ("ef", engine_->context()->input());
  EXPECT_EQ("你好", engine_->context()->commit_history().latest_text());
}

TEST_F(PartialCommitTest, LeavesPlainDigitToSelector) {
  SetUpProcessor(true);
  engine_->context()->set_input("你好");

  EXPECT_EQ(kNoop, processor_->ProcessKeyEvent(KeyEvent(XK_2, 0)));
  EXPECT_EQ("你好", engine_->context()->input());
}

}  // namespace
}  // namespace rime
