#pragma once

#include "StarPane.hpp"
#include "StarInventoryTypes.hpp"
#include "StarItemDescriptor.hpp"
#include "StarPlayerTech.hpp"
#include "StarGameTimers.hpp"
#include "StarContainerInteractor.hpp"

namespace Star {

STAR_CLASS(MainInterface);
STAR_CLASS(UniverseClient);
STAR_CLASS(Player);
STAR_CLASS(Item);
STAR_CLASS(ItemSlotWidget);
STAR_CLASS(ItemGridWidget);
STAR_CLASS(ImageWidget);
STAR_CLASS(Widget);
STAR_CLASS(ButtonWidget);
STAR_CLASS(InventoryPane);

class InventoryPane : public Pane {
public:
  InventoryPane(MainInterface* parent, PlayerPtr player, ContainerInteractorPtr containerInteractor);

  void displayed() override;
  PanePtr createTooltip(Vec2I const& screenPosition) override;
  bool sendEvent(InputEvent const& event) override;

  bool giveContainerResult(ContainerResult result);

  // update only item grids, to see if they have had their slots changed
  // this is a little hacky and should probably be checked in the player inventory instead
  void updateItems();
  bool containsNewItems() const;
  void clearChangedSlots();
  // Called by MainInterface even when the inventory pane has been dismissed,
  // so an in-flight server reply always returns leftovers to the player.
  void updateMaterialDeposit();

protected:
  virtual void update(float dt) override;

  void selectTab(String const& selected);

private:
  void organizeSelectedBag(bool sort);
  void startMaterialDeposit();
  bool quickActionsBusy() const;
  struct MaterialDeposit {
    InventorySlot source;
    ItemPtr original;
    RpcPromise<ItemPtr> result;
  };
  List<InventorySlot> m_materialDepositSlots;
  Maybe<MaterialDeposit> m_materialDeposit;
  EntityId m_materialDepositContainer = NullEntityId;
  List<ButtonWidgetPtr> m_quickActionButtons;

  MainInterface* m_parent;
  PlayerPtr m_player;
  ContainerInteractorPtr m_containerInteractor;

  bool m_alwaysDisplayCosmetics;
  bool m_displayingCosmetics;
  bool m_expectingSwap;
  InventorySlot m_containerSource;

  GameTimer m_trashBurn;
  ItemSlotWidgetPtr m_trashSlot;

  Map<String, ItemGridWidgetPtr> m_itemGrids;
  Map<String, String> m_tabButtonData;

  Map<String, WidgetPtr> m_newItemMarkers;
  String m_selectedTab;

  StringList m_pickUpSounds;
  StringList m_putDownSounds;
  StringList m_someUpSounds;
  StringList m_someDownSounds;
  Maybe<ItemDescriptor> m_currentSwapSlotItem;

  List<ImageWidgetPtr> m_disabledTechOverlays;
};

}
