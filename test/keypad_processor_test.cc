//
// Copyright RIME Developers
// Distributed under the BSD License
//

#include <gtest/gtest.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/schema.h>
#include <rime/gear/keypad_processor.h>

namespace rime {
namespace {

class KeypadProcessorTest : public ::testing::Test {
 protected:
  void SetUpProcessor(const string& mode) {
    engine_.reset(Engine::Create());
    auto* config = new Config;
    config->SetString("keypad_processor/mode", mode);
    engine_->ApplySchema(new Schema("keypad_processor_test", config));
    processor_.reset(new KeypadProcessor(Ticket(engine_.get(), "processor")));
  }

  the<Engine> engine_;
  the<KeypadProcessor> processor_;
};

TEST_F(KeypadProcessorTest, ComposeModePushesKeypadDigit) {
  SetUpProcessor("compose");

  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_KP_1, 0)));
  EXPECT_EQ("1", engine_->context()->input());
}

TEST_F(KeypadProcessorTest, AutoModePassesIdleKeypadDigit) {
  SetUpProcessor("auto");

  EXPECT_EQ(kNoop, processor_->ProcessKeyEvent(KeyEvent(XK_KP_1, 0)));
  EXPECT_TRUE(engine_->context()->input().empty());
}

TEST_F(KeypadProcessorTest, AutoModePushesWhileComposing) {
  SetUpProcessor("auto");
  engine_->context()->set_input("ni");

  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_KP_2, 0)));
  EXPECT_EQ("ni2", engine_->context()->input());
  ASSERT_FALSE(engine_->context()->composition().empty());
  EXPECT_TRUE(engine_->context()->composition().front().HasTag("raw"));
}

TEST_F(KeypadProcessorTest, AutoModeAppendsToNumericInput) {
  SetUpProcessor("auto");
  engine_->context()->set_input("1");

  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_KP_2, 0)));
  EXPECT_EQ("12", engine_->context()->input());
}

TEST_F(KeypadProcessorTest, KeypadDigitsKeepLetterPrefixRaw) {
  SetUpProcessor("auto");
  engine_->context()->set_input("ni");

  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_KP_4, 0)));
  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_KP_5, 0)));
  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_KP_6, 0)));
  EXPECT_EQ("ni456", engine_->context()->input());
  EXPECT_EQ("ni456", engine_->context()->GetCommitText());
}

TEST_F(KeypadProcessorTest, ComposeModeKeepsFollowingMainKeysRaw) {
  SetUpProcessor("compose");

  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_KP_4, 0)));
  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_a, 0)));
  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_5, 0)));
  EXPECT_EQ("4a5", engine_->context()->input());
  EXPECT_EQ("4a5", engine_->context()->GetCommitText());
}

TEST_F(KeypadProcessorTest, BackspaceKeepsRawMode) {
  SetUpProcessor("compose");

  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_KP_4, 0)));
  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_a, 0)));
  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_BackSpace, 0)));
  EXPECT_EQ("4", engine_->context()->input());
  EXPECT_EQ(kAccepted, processor_->ProcessKeyEvent(KeyEvent(XK_b, 0)));
  EXPECT_EQ("4b", engine_->context()->input());
  EXPECT_EQ("4b", engine_->context()->GetCommitText());
}

TEST_F(KeypadProcessorTest, SelectModePassesKeypadDigit) {
  SetUpProcessor("select");

  EXPECT_EQ(kNoop, processor_->ProcessKeyEvent(KeyEvent(XK_KP_3, 0)));
  EXPECT_TRUE(engine_->context()->input().empty());
}

TEST_F(KeypadProcessorTest, ModifiersAndReleasePassThrough) {
  SetUpProcessor("compose");

  EXPECT_EQ(kNoop, processor_->ProcessKeyEvent(KeyEvent(XK_KP_1, kShiftMask)));
  EXPECT_EQ(kNoop,
            processor_->ProcessKeyEvent(KeyEvent(XK_KP_1, kReleaseMask)));
  EXPECT_TRUE(engine_->context()->input().empty());
}

TEST_F(KeypadProcessorTest, MainKeyboardDigitIsPassedToSelector) {
  SetUpProcessor("compose");

  EXPECT_EQ(kNoop, processor_->ProcessKeyEvent(KeyEvent(XK_1, 0)));
  EXPECT_TRUE(engine_->context()->input().empty());
}

}  // namespace
}  // namespace rime
