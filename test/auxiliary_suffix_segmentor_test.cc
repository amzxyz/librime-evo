//
// Copyright RIME Developers
// Distributed under the BSD License
//

#include <gtest/gtest.h>
#include <rime/config.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/ticket.h>
#include <rime/gear/auxiliary_suffix_segmentor.h>

using namespace rime;

namespace {

Schema MakeSchema() {
  auto* config = new Config;
  config->SetString("translator/aux_suffix", "`");
  return Schema("auxiliary_suffix_test", config);
}

}  // namespace

TEST(AuxiliarySuffixSegmentorTest, KeepsSuffixOutOfMainSegment) {
  Schema schema = MakeSchema();
  AuxiliarySuffixSegmentor segmentor(
      Ticket(&schema, "auxiliary_suffix_segmentor"));
  Segmentation segmentation;
  segmentation.Reset("ni`z");

  EXPECT_FALSE(segmentor.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_EQ(0U, segmentation[0].start);
  EXPECT_EQ(2U, segmentation[0].end);
  EXPECT_TRUE(segmentation[0].HasTag("abc"));
  EXPECT_TRUE(segmentation[0].HasTag("auxiliary_suffix"));

  EXPECT_TRUE(segmentation.Forward());
  EXPECT_FALSE(segmentor.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_EQ(0U, segmentation[0].start);
  EXPECT_EQ(2U, segmentation[0].end);
  EXPECT_TRUE(segmentation[0].HasTag("auxiliary_suffix"));
}

TEST(AuxiliarySuffixSegmentorTest, ReopensConfirmedMainSegment) {
  Schema schema = MakeSchema();
  AuxiliarySuffixSegmentor segmentor(
      Ticket(&schema, "auxiliary_suffix_segmentor"));
  Segmentation segmentation;
  segmentation.Reset("ni");
  Segment main(0, 2);
  main.status = Segment::kConfirmed;
  main.tags.insert("abc");
  segmentation.AddSegment(main);
  segmentation.Forward();

  segmentation.Reset("ni`z");
  EXPECT_FALSE(segmentor.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_EQ(Segment::kVoid, segmentation[0].status);
  EXPECT_TRUE(segmentation[0].HasTag("auxiliary_suffix"));
  EXPECT_TRUE(segmentation[0].menu == nullptr);
}

TEST(AuxiliarySuffixSegmentorTest, ClearsSuffixStateAfterDeletion) {
  Schema schema = MakeSchema();
  AuxiliarySuffixSegmentor segmentor(
      Ticket(&schema, "auxiliary_suffix_segmentor"));
  Segmentation segmentation;
  segmentation.Reset("ni`z");
  EXPECT_FALSE(segmentor.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_TRUE(segmentation[0].HasTag("auxiliary_suffix"));

  segmentation.Reset("ni");
  EXPECT_TRUE(segmentor.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_TRUE(segmentation[0].HasTag("abc"));
  EXPECT_FALSE(segmentation[0].HasTag("auxiliary_suffix"));
}
