#include "../windowing/StarTooltipLayout.hpp"

#include "gtest/gtest.h"

using namespace Star;

TEST(TooltipLayout, KeepsPreferredPositionWhenItFits) {
  auto layout = tooltipLayout({1280, 720}, 2, {100, 80}, {100, 200}, {10, 10});
  EXPECT_FLOAT_EQ(2, layout.scale);
  EXPECT_EQ(Vec2I(110, 130), layout.position);
}

TEST(TooltipLayout, FlippedTooltipStillFitsNearTopAndLeftEdges) {
  // Flipping above the pointer at the bottom used to overflow the top.
  auto layout = tooltipLayout({800, 500}, 1, {700, 400}, {300, 200}, {10, 10});
  EXPECT_FLOAT_EQ(1, layout.scale);
  EXPECT_EQ(Vec2I(0, 100), layout.position);
}

TEST(TooltipLayout, FitsModdedTooltipsWithoutChangingNormalScale) {
  for (auto screen : {Vec2U(854, 480), Vec2U(1280, 720), Vec2U(2340, 1080), Vec2U(720, 1280)}) {
    for (float scale : {1.0f, 1.35f, 2.0f, 3.0f, 4.0f}) {
      for (auto size : {Vec2I(240, 180), Vec2I(400, 600), Vec2I(1500, 100)}) {
        for (auto mouse : {Vec2I(), Vec2I(10, 10), Vec2I(300, 200), Vec2I(screen)}) {
          auto layout = tooltipLayout(screen, scale, size, mouse, {10, 10});
          EXPECT_GT(layout.scale, 0);
          EXPECT_LE(layout.scale, scale);
          EXPECT_GE(layout.position[0], 0);
          EXPECT_GE(layout.position[1], 0);
          for (size_t axis = 0; axis < 2; ++axis)
            EXPECT_LE((layout.position[axis] + size[axis]) * layout.scale, screen[axis] + 0.001f);
        }
      }
    }
  }
}
