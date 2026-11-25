// Copyright RIME Developers
// Distributed under the BSD License
#include <gtest/gtest.h>
#include <rime/context.h>

using namespace rime;

TEST(CodeChoiceTest, ReplaceInputPreservesSuffixAndNotifiesOnce) {
  Context context;
  context.set_input("64426tail");
  context.set_caret_pos(5);
  int updates = 0;
  context.update_notifier().connect([&](Context*) { ++updates; });
  EXPECT_TRUE(context.ReplaceInput(0, 2, "ni'"));
  EXPECT_EQ("ni'426tail", context.input());
  EXPECT_EQ(6, context.caret_pos());
  EXPECT_EQ(1, updates);
  EXPECT_FALSE(context.ReplaceInput(6, 8, "x"));
  EXPECT_FALSE(context.ReplaceInput(0, 3, "ni'"));
  EXPECT_EQ(1, updates);
}
