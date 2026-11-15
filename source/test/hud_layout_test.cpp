#include "../windowing/StarHudLayout.hpp"
#include "gtest/gtest.h"

using namespace Star;

TEST(HudLayoutTest, MobileHotbarFitsLargeUiScaleAndKeepsWidgetHitGeometry) {
  // FU on a 1000px viewport with 2x UI scale: the enlarged 450-unit
  // hotbar must leave the right corner buttons clear.
  float scale = mobileHotbarScale(1000.0f / 2.0f, 450.0f);
  EXPECT_NEAR(0.89f, scale, 0.01f);
  EXPECT_LE(450.0f * scale + 96.0f, 500.0f);
  EXPECT_EQ(1.0f, mobileHotbarScale(1280.0f / 1.44f, 450.0f));
  auto layout = scaleHudLayout(JsonObject{
    {"paneLayout", JsonObject{{"slot", JsonObject{
      {"position", JsonArray{20, 10}}, {"size", JsonArray{25, 25}},
      {"itemScale", 1.25f}, {"backingImage", "/slot.png?scalenearest=1.25"},
      {"callback", "customBar1L"}}}}}, {"countMidAnchor", JsonArray{12, -8}}}, scale);
  EXPECT_EQ(18, layout.query("paneLayout.slot.position[0]").toInt());
  EXPECT_EQ(22, layout.query("paneLayout.slot.size[0]").toInt());
  EXPECT_NEAR(1.25f * scale, layout.queryFloat("paneLayout.slot.itemScale"), 0.001f);
  EXPECT_EQ("customBar1L", layout.queryString("paneLayout.slot.callback"));
}

TEST(HudLayoutTest, NarrowLayoutClearsCornerButtonsAndWideLayoutStaysCentered) {
  auto offset = mobileHotbarOffset(500, 450);
  EXPECT_EQ(Vec2I(-40, -100), offset);
  float width = 450 * mobileHotbarScale(500, 450);
  float left = (500 - width) / 2 + offset[0];
  EXPECT_GE(left, 8);
  EXPECT_LE(left + width, 500 - 88);
  EXPECT_EQ(Vec2I(), mobileHotbarOffset(900, 450));
}
