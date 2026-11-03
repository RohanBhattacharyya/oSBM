#pragma once

#include "StarTouchInput.hpp"

#include "StarInventoryTypes.hpp"
#include "StarMainInterfaceTypes.hpp"

namespace Star {

STAR_CLASS(MainInterface);
STAR_CLASS(UniverseClient);
STAR_CLASS(Player);
STAR_CLASS(Item);
STAR_CLASS(ItemSlotWidget);
STAR_CLASS(ImageWidget);

STAR_CLASS(ActionBar);

class ActionBar : public Pane {
public:
  ActionBar(MainInterfacePaneManager* paneManager, PlayerPtr player);

  PanePtr createTooltip(Vec2I const& screenPosition) override;
  bool sendEvent(InputEvent const& event) override;

  void update(float dt) override;

  void swapHotbar();

  Maybe<String> cursorOverride(Vec2I const& screenPosition) override;

private:
  struct CustomBarEntry {
    ItemSlotWidgetPtr left;
    ItemSlotWidgetPtr right;
    ImageWidgetPtr leftOverlay;
    ImageWidgetPtr rightOverlay;
  };

  void customBarClick(uint8_t index, bool primary);
  void customBarClickRight(uint8_t index, bool primary);
  void essentialBarClick(uint8_t index);
  void swapCustomBar();

  // Touch-friendly hotbar: dragging a finger sideways along the bar steps the
  // selected slot (like a mouse wheel), so you do not have to hit tiny slots.
  bool trackHotbarSwipe(InputEvent const& event);
  bool positionInHotbar(Vec2I const& position) const;

  bool m_swipeTracking = false;
  bool m_swipeConsumed = false;
  int m_swipeLastX = 0;
  TouchInput::Swipe m_hotbarSwipe;

  MainInterfacePaneManager* m_paneManager;
  PlayerPtr m_player;
  Json m_config;

  Vec2I m_actionBarSelectOffset;
  StringList m_switchSounds;

  List<CustomBarEntry> m_customBarWidgets;
  ImageWidgetPtr m_customSelectedWidget;

  List<ItemSlotWidgetPtr> m_essentialBarWidgets;
  ImageWidgetPtr m_essentialSelectedWidget;

  SelectedActionBarLocation m_emptyHandsPreviousActionBarLocation;
  Maybe<pair<CustomBarIndex, bool>> m_customBarHover;
};

}
