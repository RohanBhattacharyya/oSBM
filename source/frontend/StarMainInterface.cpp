#include "StarMainInterface.hpp"
#include "StarJsonExtra.hpp"
#include "StarLogging.hpp"
#include "StarLexicalCast.hpp"
#include "StarContainerInterface.hpp"
#include "StarCraftingInterface.hpp"
#include "StarMerchantInterface.hpp"
#include "StarRoot.hpp"
#include "StarUniverseClient.hpp"
#include "StarCodexInterface.hpp"
#include "StarSongbookInterface.hpp"
#include "StarQuestInterface.hpp"
#include "StarQuestManager.hpp"
#include "StarPopupInterface.hpp"
#include "StarConfirmationDialog.hpp"
#include "StarHttpTrustDialog.hpp"
#include "StarJoinRequestDialog.hpp"
#include "StarImageMetadataDatabase.hpp"
#include "StarGuiReader.hpp"
#include "StarGuiTypes.hpp"
#include "StarPaneManager.hpp"
#include "StarClientCommandProcessor.hpp"
#include "StarChat.hpp"
#include "StarOptionsMenu.hpp"
#include "StarActionBar.hpp"
#include "StarWireInterface.hpp"
#include "StarTeamBar.hpp"
#include "StarStatusPane.hpp"
#include "StarCanvasWidget.hpp"
#include "StarLabelWidget.hpp"
#include "StarItemSlotWidget.hpp"
#include "StarButtonWidget.hpp"
#include "StarPlayer.hpp"
#include "StarPlayerLog.hpp"
#include "StarMonster.hpp"
#include "StarItemDrop.hpp"
#include "StarAssets.hpp"
#include "StarPlayerInventory.hpp"
#include "StarCelestialDatabase.hpp"
#include "StarItem.hpp"
#include "StarAiInterface.hpp"
#include "StarDrawable.hpp"
#include "StarFireableItem.hpp"
#include "StarClientContext.hpp"
#include "StarToolUserEntity.hpp"
#include "StarTeleportDialog.hpp"
#include "StarCinematic.hpp"
#include "StarNameplatePainter.hpp"
#include "StarQuestIndicatorPainter.hpp"
#include "StarScriptPane.hpp"
#include "StarContainerEntity.hpp"
#include "StarWarpTargetEntity.hpp"
#include "StarPlayerUniverseMap.hpp"
#include "StarWorldTemplate.hpp"
#include "StarRadioMessagePopup.hpp"
#include "StarAiTypes.hpp"
#include "StarActiveItem.hpp"
#include "StarInspectionTool.hpp"
#include "StarQuestTracker.hpp"
#include "StarContainerInteractor.hpp"
#include "StarChatBubbleManager.hpp"
#include "StarNpc.hpp"
#include "StarCharSelection.hpp"

#include <cmath>

namespace Star {

static String actionWheelShortCount(uint64_t n) {
  if (n < 10000)
    return toString(n);

  uint64_t divisor = 1000ull;
  char suffix = 'k';
  if (n >= 1000000000000000000ull) {
    divisor = 1000000000000000000ull;
    suffix = 'Q';
  } else if (n >= 1000000000000000ull) {
    divisor = 1000000000000000ull;
    suffix = 'q';
  } else if (n >= 1000000000000ull) {
    divisor = 1000000000000ull;
    suffix = 't';
  } else if (n >= 1000000000ull) {
    divisor = 1000000000ull;
    suffix = 'b';
  } else if (n >= 1000000ull) {
    divisor = 1000000ull;
    suffix = 'm';
  }

  uint64_t whole = n / divisor;
  if (whole >= 100ull)
    return strf("{}{:c}", whole, suffix);

  uint64_t frac = (n - whole * divisor) / (divisor / 1000);
  if (frac == 0)
    return strf("{}{:c}", whole, suffix);
  else if (whole >= 10)
    return strf("{}.{}{:c}", whole, frac / 100, suffix);
  else
    return strf("{}.{:02d}{:c}", whole, frac / 10, suffix);
}

GuiMessage::GuiMessage() : message(), cooldown(), springState() {}

GuiMessage::GuiMessage(String const& message, float cooldown, float spring)
  : message(message), cooldown(cooldown), springState(spring) {}

MainInterface::MainInterface(UniverseClientPtr client, WorldPainterPtr painter, CinematicPtr cinematicOverlay)
  : m_guiContext(GuiContext::singletonPtr())
  , m_config(MainInterfaceConfig::loadFromAssets())
  , m_client(std::move(client))
  , m_worldPainter(std::move(painter))
  , m_cinematicOverlay(std::move(cinematicOverlay))
  , m_containerInteractor(make_shared<ContainerInteractor>())
{
  GuiReader itemSlotReader;
  m_cursorItem = convert<ItemSlotWidget>(itemSlotReader.makeSingle("cursorItemSlot", m_config->cursorItemSlot));

  m_planetNameTimer = GameTimer(m_config->planetNameTime);

  m_debugSpatialClearTimer = GameTimer(m_config->debugSpatialClearTime);
  m_debugMapClearTimer = GameTimer(m_config->debugMapClearTime);

  m_stickyTargetingTimer = GameTimer(m_config->monsterHealthBarTime);

  m_inventoryWindow = make_shared<InventoryPane>(this, m_client->mainPlayer(), m_containerInteractor);
  m_paneManager.registerPane(MainInterfacePanes::Inventory, PaneLayer::Window, m_inventoryWindow, [this](PanePtr const&) {
      if (auto player = m_client->mainPlayer())
          player->clearSwap();
      if (m_containerPane) {
        m_containerPane->dismiss();
        m_containerPane = {};
        m_containerInteractor->closeContainer();
      }
      for (EntityId id : m_interactionScriptPanes.keys()) {
        if (m_paneManager.isDisplayed(m_interactionScriptPanes[id]) && as<ScriptPane>(m_interactionScriptPanes[id])->openWithInventory())
          m_interactionScriptPanes[id]->dismiss();
      }
    });

  m_overflowMessage = make_shared<GuiMessage>("", 0);

  m_plainCraftingWindow = make_shared<CraftingPane>(m_client->worldClient(), m_client->mainPlayer(), JsonObject{{"filter", JsonArray{"plain"}}}, m_client->mainPlayer()->entityId());
  m_paneManager.registerPane(MainInterfacePanes::CraftingPlain, PaneLayer::Window, m_plainCraftingWindow);

  m_paneManager.registerPane(MainInterfacePanes::EscapeDialog, PaneLayer::ModalWindow, createEscapeDialog());

  auto songbookInterface = make_shared<SongbookInterface>(m_client->mainPlayer());
  m_paneManager.registerPane(MainInterfacePanes::Songbook, PaneLayer::Window, songbookInterface);

  m_questLogInterface = make_shared<QuestLogInterface>(m_client->questManager(), m_client->mainPlayer(), m_cinematicOverlay, m_client);
  m_paneManager.registerPane(MainInterfacePanes::QuestLog, PaneLayer::Window, m_questLogInterface);

  auto aiInterface = make_shared<AiInterface>(m_client, m_cinematicOverlay, &m_paneManager);
  m_paneManager.registerPane(MainInterfacePanes::Ai, PaneLayer::Window, aiInterface);

  m_codexInterface = make_shared<CodexInterface>(m_client->mainPlayer());
  m_paneManager.registerPane(MainInterfacePanes::Codex, PaneLayer::Window, m_codexInterface);

  m_optionsMenu = make_shared<OptionsMenu>(&m_paneManager,m_client);
  m_paneManager.registerPane(MainInterfacePanes::Options, PaneLayer::ModalWindow, m_optionsMenu);

  m_popupInterface = make_shared<PopupInterface>();
  m_paneManager.registerPane(MainInterfacePanes::Popup, PaneLayer::Window, m_popupInterface);

  m_confirmationDialog = make_shared<ConfirmationDialog>();
  m_paneManager.registerPane(MainInterfacePanes::Confirmation, PaneLayer::ModalWindow, m_confirmationDialog);

  initHttpTrustDialog();

  m_joinRequestDialog = make_shared<JoinRequestDialog>();
  m_paneManager.registerPane(MainInterfacePanes::JoinRequest, PaneLayer::ModalWindow, m_joinRequestDialog);

  m_actionBar = make_shared<ActionBar>(&m_paneManager, m_client->mainPlayer());
  m_paneManager.registerPane(MainInterfacePanes::ActionBar, PaneLayer::Hud, m_actionBar);

  m_questTracker = make_shared<QuestTrackerPane>();
  m_paneManager.registerPane(MainInterfacePanes::QuestTracker, PaneLayer::Hud, m_questTracker);

  m_mmUpgrade = make_shared<ScriptPane>(m_client, "/interface/scripted/mmupgrade/mmupgradegui.config");
  m_paneManager.registerPane(MainInterfacePanes::MmUpgrade, PaneLayer::Window, m_mmUpgrade);

  m_collections = make_shared<ScriptPane>(m_client, "/interface/scripted/collections/collectionsgui.config");
  m_paneManager.registerPane(MainInterfacePanes::Collections, PaneLayer::Window, m_collections);

  m_chat = make_shared<Chat>(m_client, Root::singleton().assets()->json("/interface/chat/chat.config"));
  m_paneManager.registerPane(MainInterfacePanes::Chat, PaneLayer::Hud, m_chat);
  m_clientCommandProcessor = make_shared<ClientCommandProcessor>(m_client, m_cinematicOverlay, &m_paneManager, m_config->macroCommands);

  m_radioMessagePopup = make_shared<RadioMessagePopup>();
  m_paneManager.registerPane(MainInterfacePanes::RadioMessagePopup, PaneLayer::Hud, m_radioMessagePopup);

  m_wireInterface = make_shared<WirePane>(m_client->worldClient(), m_client->mainPlayer(), m_worldPainter);
  m_paneManager.registerPane(MainInterfacePanes::WireInterface, PaneLayer::World, m_wireInterface);
  m_client->mainPlayer()->setWireConnector(m_wireInterface.get());

  auto teamBar = make_shared<TeamBar>(this, m_client);
  m_paneManager.registerPane(MainInterfacePanes::TeamBar, PaneLayer::Hud, teamBar);

  auto statusPane = make_shared<StatusPane>(&m_paneManager, m_client);
  m_paneManager.registerPane(MainInterfacePanes::StatusPane, PaneLayer::Hud, statusPane);

  auto planetName = make_shared<Pane>();
  m_planetText = make_shared<LabelWidget>();
  m_planetText->setTextStyle(m_config->planetNameTextStyle);
  m_planetText->setAnchor(HorizontalAnchor::HMidAnchor, VerticalAnchor::VMidAnchor);
  planetName->disableScissoring();
  planetName->setPosition(m_config->planetNameOffset);
  planetName->setAnchor(PaneAnchor::Center);
  planetName->addChild("planetText", m_planetText);
  m_paneManager.registerPane(MainInterfacePanes::PlanetText, PaneLayer::Hud, planetName);

  auto charSelectionMenu = make_shared<CharSelectionPane>(m_client->playerStorage(), [=]() {},
    [=](PlayerPtr mainPlayer) {
      m_client->switchPlayer(mainPlayer->uuid());
      auto configuration = Root::singleton().configuration();
      if (configuration->get("characterSwapMovesToFront", false).toBool())
        m_client->playerStorage()->moveToFront(mainPlayer->uuid());
      if (configuration->get("characterSwapDismisses", false).toBool())
        m_paneManager.dismissRegisteredPane(MainInterfacePanes::CharacterSwap);
    }, [=](Uuid) {});
  charSelectionMenu->setReadOnly(true);
  charSelectionMenu->setAnchor(PaneAnchor::Center);
  charSelectionMenu->unlockPosition();
  auto backgrounds = charSelectionMenu->getBG();
  backgrounds.header = std::move(backgrounds.body);
  charSelectionMenu->setBG(backgrounds);
  charSelectionMenu->findChild("toggleDismissLabel")->setVisibility(true);
  auto toggleDismiss = charSelectionMenu->findChild<ButtonWidget>("toggleDismissCheckbox");
  auto configuration = Root::singleton().configuration();
  toggleDismiss->setChecked(configuration->get("characterSwapDismisses", false).toBool());
  toggleDismiss->setVisibility(true);

  m_paneManager.registerPane(MainInterfacePanes::CharacterSwap, PaneLayer::Window, charSelectionMenu);

  m_nameplatePainter = make_shared<NameplatePainter>();
  m_questIndicatorPainter = make_shared<QuestIndicatorPainter>(m_client);
  m_chatBubbleManager = make_shared<ChatBubbleManager>();
}

MainInterface::~MainInterface() {
  m_paneManager.dismissAllPanes();
}

MainInterface::RunningState MainInterface::currentState() const {
  return m_state;
}

MainInterfacePaneManager* MainInterface::paneManager() {
  return &m_paneManager;
}

bool MainInterface::escapeDialogOpen() const {
  return m_paneManager.registeredPaneIsDisplayed(MainInterfacePanes::EscapeDialog) || m_paneManager.registeredPaneIsDisplayed(MainInterfacePanes::Options);
}

void MainInterface::openCraftingWindow(Json const& config, EntityId sourceEntityId) {
  if (m_craftingWindow && m_paneManager.isDisplayed(m_craftingWindow)) {
    m_paneManager.dismissPane(m_craftingWindow);
    if (sourceEntityId != NullEntityId && m_craftingWindow->sourceEntityId() == sourceEntityId) {
      m_craftingWindow.reset();
      return;
    }
  }

  m_craftingWindow = make_shared<CraftingPane>(m_client->worldClient(), m_client->mainPlayer(), config, sourceEntityId);
  m_paneManager.displayPane(PaneLayer::Window, m_craftingWindow, [this](PanePtr const&) {
    if (auto player = m_client->mainPlayer())
      player->clearSwap();
    });
}

void MainInterface::openMerchantWindow(Json const& config, EntityId sourceEntityId) {
  if (m_merchantWindow && m_paneManager.isDisplayed(m_merchantWindow)) {
    m_paneManager.dismissPane(m_merchantWindow);
    if (sourceEntityId != NullEntityId && m_merchantWindow->sourceEntityId() == sourceEntityId) {
      m_merchantWindow.reset();
      return;
    }
  }

  bool openWithInventory = config.getBool("openWithInventory", true);
  bool closeWithInventory = config.getBool("closeWithInventory", !m_paneManager.registeredPaneIsDisplayed(MainInterfacePanes::Inventory));
  m_merchantWindow = make_shared<MerchantPane>(m_client->worldClient(), m_client->mainPlayer(), config, sourceEntityId);
  m_paneManager.displayPane(PaneLayer::Window, m_merchantWindow, [this, closeWithInventory](PanePtr const&) {
    if (closeWithInventory)
      m_paneManager.dismissRegisteredPane(MainInterfacePanes::Inventory);
   });
  if (openWithInventory)
    m_paneManager.displayRegisteredPane(MainInterfacePanes::Inventory);

  m_paneManager.bringPaneAdjacent(m_paneManager.registeredPane(MainInterfacePanes::Inventory),
    m_merchantWindow, Root::singleton().assets()->json("/interface.config:bringAdjacentWindowGap").toFloat());
}

void MainInterface::togglePlainCraftingWindow() {
  m_paneManager.toggleRegisteredPane(MainInterfacePanes::CraftingPlain);

  if (m_craftingWindow && m_craftingWindow->isDisplayed()
      && m_craftingWindow != m_paneManager.registeredPane(MainInterfacePanes::CraftingPlain))
    m_paneManager.dismissPane(m_craftingWindow);

  m_craftingWindow = m_plainCraftingWindow;
}

bool MainInterface::windowsOpen() const {
  return (bool)m_paneManager.topPane({PaneLayer::Window});
}

MerchantPanePtr MainInterface::activeMerchantPane() const {
  if (m_paneManager.isDisplayed(m_merchantWindow))
    return m_merchantWindow;
  else
    return {};
}

bool MainInterface::handleInputEvent(InputEvent const& event) {
  auto player = m_client->mainPlayer();
  auto inv = player->inventory();
  auto& root = Root::singleton();

  if (auto mouseMove = event.ptr<MouseMoveEvent>()) {
    m_cursorScreenIPos = Vec2I::round(m_cursorScreenPos = mouseMove->mousePosition);
    m_cursorVisible = mouseMove->cursorVisible;
    m_mobileDirectionalAimActive = false;
    if (!mouseMove->cursorVisible)
      return false;
  }

  if (auto actionWheel = event.ptr<ActionWheelEvent>())
    return handleActionWheelEvent(*actionWheel);

  if (auto directionalAim = event.ptr<DirectionalAimEvent>())
    return handleDirectionalAimEvent(*directionalAim);

  InputEvent effectiveEvent = event;
  if (m_mobileDirectionalAimActive) {
    if (auto mouseDown = event.ptr<MouseButtonDownEvent>())
      effectiveEvent = MouseButtonDownEvent{mouseDown->mouseButton, m_cursorScreenPos};
    else if (auto mouseUp = event.ptr<MouseButtonUpEvent>())
      effectiveEvent = MouseButtonUpEvent{mouseUp->mouseButton, m_cursorScreenPos};
    else if (auto mouseWheel = event.ptr<MouseWheelEvent>())
      effectiveEvent = MouseWheelEvent{mouseWheel->mouseWheel, m_cursorScreenPos};
  }

  if (m_paneManager.sendInputEvent(effectiveEvent)) {
    if (!effectiveEvent.is<MouseButtonUpEvent>() && !effectiveEvent.is<KeyUpEvent>())
      return true;
  }

  if (effectiveEvent.is<KeyDownEvent>()) {
    auto actions = m_guiContext->actions(effectiveEvent);

    if (m_chat->hasFocus()) {
      if (actions.contains(InterfaceAction::ChatSendLine)) {
        doChat(m_chat->currentChat(), true);
        m_chat->clearCurrentChat();
        m_chat->stopChat();
        return true;
      }
    } else if (!m_paneManager.keyboardCapturedWidget()) {
      Maybe<InventorySlot> swapSlot;

      for (auto action : actions) {
        switch (action) {
          default:
            break;
          case InterfaceAction::GuiShifting:
            m_guiContext->setShiftHeld(true);
            break;
          case InterfaceAction::ChatBegin:
            m_chat->startChat();
            break;

          case InterfaceAction::InterfaceHideHud:
            m_disableHud = !m_disableHud;
            break;

          case InterfaceAction::InterfaceRepeatCommand:
            if (!m_lastCommand.empty())
              doChat(m_lastCommand, false);
            break;

          case InterfaceAction::InterfaceToggleFullscreen:
            m_optionsMenu->toggleFullscreen();
            break;

          case InterfaceAction::InterfaceReload:
            root.reload();
            root.fullyLoad();
            break;

          case InterfaceAction::ChatBeginCommand:
            m_chat->startCommand();
            break;

          case InterfaceAction::InterfaceEscapeMenu:
            m_paneManager.toggleRegisteredPane(MainInterfacePanes::EscapeDialog);
            break;

          case InterfaceAction::InterfaceInventory:
            m_paneManager.toggleRegisteredPane(MainInterfacePanes::Inventory);
            break;

          case InterfaceAction::InterfaceCodex:
            m_paneManager.toggleRegisteredPane(MainInterfacePanes::Codex);
            break;

          case InterfaceAction::InterfaceQuest:
            m_paneManager.toggleRegisteredPane(MainInterfacePanes::QuestLog);
            break;

          case InterfaceAction::InterfaceCrafting:
            togglePlainCraftingWindow();
            break;
        }
      }
    }

    return false;

  } else if (auto keyUp = effectiveEvent.ptr<KeyUpEvent>()) {
    if (m_guiContext->actionsForKey(keyUp->key).contains(InterfaceAction::GuiShifting))
      m_guiContext->setShiftHeld(false);

    return false;

  } else if (auto mouseDown = effectiveEvent.ptr<MouseButtonDownEvent>()) {
    auto mouseButton = mouseDown->mouseButton;
    if (mouseButton >= MouseButton::Left && mouseButton <= MouseButton::Right
    && overlayClick(mouseDown->mousePosition, mouseDown->mouseButton)) {
      return true;
    } else {
      if (mouseButton == MouseButton::Left)
        player->beginPrimaryFire();
      if (mouseButton == MouseButton::Right)
        player->beginAltFire();
      if (mouseButton == MouseButton::Middle)
        player->beginTrigger();
    }
  } else if (auto mouseUp = effectiveEvent.ptr<MouseButtonUpEvent>()) {
    if (mouseUp->mouseButton == MouseButton::Left)
      player->endPrimaryFire();
    if (mouseUp->mouseButton == MouseButton::Right)
      player->endAltFire();
    if (mouseUp->mouseButton == MouseButton::Middle)
      player->endTrigger();
  }

  bool captured = false;

  for (auto& pair : m_canvases)
    captured |= pair.second->sendEvent(effectiveEvent);

  return captured;
}

bool MainInterface::inputFocus() const {
  return (bool)m_paneManager.keyboardCapturedWidget();
}

bool MainInterface::textInputActive() const {
  return m_paneManager.keyboardCapturedForTextInput();
}

void MainInterface::handleInteractAction(InteractAction interactAction) {
  auto assets = Root::singleton().assets();
  auto world = m_client->worldClient();

  if (interactAction.type == InteractActionType::OpenContainer) {
    // If we're currently displaying this container, close it.
    if (m_containerPane && m_containerInteractor->openContainerId() == interactAction.entityId) {
      m_paneManager.dismissPane(m_containerPane);
      return;
    }

    // If we're currently displaying another container, close it before we open.
    if (m_containerPane)
      m_paneManager.dismissPane(m_containerPane);

    auto containerEntity = world->get<ContainerEntity>(interactAction.entityId);
    if (!containerEntity)
      return;

    m_containerInteractor->openContainer(containerEntity);

    bool closeWithInventory = !m_paneManager.registeredPaneIsDisplayed(MainInterfacePanes::Inventory);
    m_paneManager.displayRegisteredPane(MainInterfacePanes::Inventory);

    m_containerPane = make_shared<ContainerPane>(world, m_client->mainPlayer(), m_containerInteractor);
    m_paneManager.displayPane(PaneLayer::Window, m_containerPane, [this, closeWithInventory](PanePtr const&) {
      if (closeWithInventory)
        m_paneManager.dismissRegisteredPane(MainInterfacePanes::Inventory);
      else {
        m_containerInteractor->closeContainer();
        m_containerPane = {};
      }
    });

    m_paneManager.bringPaneAdjacent(m_paneManager.registeredPane(MainInterfacePanes::Inventory),
        m_containerPane, Root::singleton().assets()->json("/interface.config:bringAdjacentWindowGap").toFloat());
  } else if (interactAction.type == InteractActionType::SitDown) {
    m_client->mainPlayer()->lounge(interactAction.entityId, interactAction.data.toUInt());
  } else if (interactAction.type == InteractActionType::OpenCraftingInterface) {
    if (interactAction.entityId != NullEntityId && !world->entity(interactAction.entityId))
      return;

    openCraftingWindow(interactAction.data, interactAction.entityId);
  } else if (interactAction.type == InteractActionType::OpenSongbookInterface) {
    m_paneManager.displayRegisteredPane(MainInterfacePanes::Songbook);
  } else if (interactAction.type == InteractActionType::OpenNpcCraftingInterface) {
    if (interactAction.entityId != NullEntityId && !world->entity(interactAction.entityId))
      return;
    // wait, this is literally the exact same as OpenCraftingInterface. what the fuck? lol
    openCraftingWindow(interactAction.data, interactAction.entityId);
  } else if (interactAction.type == InteractActionType::OpenMerchantInterface) {
    if (interactAction.entityId != NullEntityId && !world->entity(interactAction.entityId))
      return;

    openMerchantWindow(interactAction.data, interactAction.entityId);
  } else if (interactAction.type == InteractActionType::OpenAiInterface) {
    as<AiInterface>(m_paneManager.registeredPane(MainInterfacePanes::Ai))->setSourceEntityId(interactAction.entityId);
    m_paneManager.displayRegisteredPane(MainInterfacePanes::Ai);
  } else if (interactAction.type == InteractActionType::OpenTeleportDialog) {
    if (m_teleportDialog)
      m_teleportDialog->dismiss();

    if (!m_client->canTeleport())
      return;

    auto currentLocation = TeleportBookmark();

    auto config = assets->fetchJson(interactAction.data);
    if (config.getBool("canBookmark", false)) {
      if (auto entity = world->entity(interactAction.entityId)) {
        if (auto uniqueEntityId = entity->uniqueId()) {
          auto worldTemplate = m_client->worldClient()->currentTemplate();

          String icon, planetName;
          if (m_client->playerWorld().is<ClientShipWorldId>()) {
            icon = "ship";
            planetName = "Player Ship";
          } else if (m_client->playerWorld().is<CelestialWorldId>()) {
            icon = worldTemplate->worldParameters()->typeName;
            planetName = worldTemplate->worldName();
          } else if (m_client->playerWorld().is<InstanceWorldId>()) {
            icon = worldTemplate->worldParameters()->typeName;
            planetName = worldTemplate->worldName();
          } else {
            icon = "default";
            planetName = "???";
          }

          currentLocation = TeleportBookmark {
            {m_client->playerWorld(), SpawnTargetUniqueEntity(*uniqueEntityId)},
            planetName,
            config.getString("bookmarkName", ""),
            icon
          };

          if (!m_client->mainPlayer()->universeMap()->teleportBookmarks().contains(currentLocation) || !config.getBool("canTeleport", true)) {
            auto editBookmarkDialog = make_shared<EditBookmarkDialog>(m_client->mainPlayer()->universeMap());
            editBookmarkDialog->setBookmark(currentLocation);
            m_paneManager.displayPane(PaneLayer::ModalWindow, editBookmarkDialog);
            return;
          }
        }
      }
    }

    if (config.getBool("canTeleport", true)) {
      m_teleportDialog = make_shared<TeleportDialog>(m_client, &m_paneManager, interactAction.data, interactAction.entityId, currentLocation);
      m_paneManager.displayPane(PaneLayer::ModalWindow, m_teleportDialog);
    }
  } else if (interactAction.type == InteractActionType::ShowPopup) {
    m_paneManager.displayRegisteredPane(MainInterfacePanes::Popup);
    m_popupInterface->displayMessage(interactAction.data.getString("message"), interactAction.data.getString("title", ""), interactAction.data.getString("subtitle", ""), interactAction.data.optString("sound"));
  } else if (interactAction.type == InteractActionType::ScriptPane) {
    auto sourceEntity = interactAction.entityId;
    // dismiss if there's already a scriptpane open for this source entity
    if (sourceEntity != NullEntityId && m_interactionScriptPanes.contains(sourceEntity) && m_paneManager.isDisplayed(m_interactionScriptPanes[sourceEntity]))
      m_paneManager.dismissPane(m_interactionScriptPanes[sourceEntity]);

    ScriptPanePtr scriptPane = make_shared<ScriptPane>(m_client, interactAction.data, sourceEntity);
    displayScriptPane(scriptPane, sourceEntity);

  } else if (interactAction.type == InteractActionType::Message) {
    m_client->mainPlayer()->receiveMessage(connectionForEntity(interactAction.entityId),
        interactAction.data.getString("messageType"), interactAction.data.getArray("messageArgs"));
  }
}

void MainInterface::preUpdate(float) {
  auto player = m_client->mainPlayer();
  if (!m_client->paused())
    player->aim(cursorWorldPosition());

  if (m_paneManager.topPane({PaneLayer::Window, PaneLayer::ModalWindow}))
    player->setBusyState(PlayerBusyState::Menu);
  else if (m_chat->hasFocus())
    player->setBusyState(PlayerBusyState::Chatting);
  else
    player->setBusyState(PlayerBusyState::None);
}

void MainInterface::update(float dt) {
  m_paneManager.update(dt);
  m_cursor.update(dt);
  m_cursorDraggingCached = (bool)m_client->mainPlayer()->inventory()->swapSlotItem();

  m_questLogInterface->pollDialog(&m_paneManager);

  if (!m_paneManager.topPane({PaneLayer::ModalWindow}) && m_codexInterface->showNewCodex())
    m_paneManager.displayRegisteredPane(MainInterfacePanes::Codex);

  auto player = m_client->mainPlayer();
  auto cursorWorldPos = cursorWorldPosition();
  if (player->wireToolInUse()) {
    m_paneManager.displayRegisteredPane(MainInterfacePanes::WireInterface);
    player->setWireConnector(m_wireInterface.get());
  } else {
    m_paneManager.dismissRegisteredPane(MainInterfacePanes::WireInterface);
  }

  // update inventory pane items, to know if item slots changed. Only needed while
  // the inventory is actually open -- updateItems() rebuilds a String list of every
  // slot name across all bags every frame for change detection, which is wasteful
  // (and notable for large modded inventories) when the pane isn't even visible.
  if (m_paneManager.isDisplayed(m_inventoryWindow))
    m_inventoryWindow->updateItems();

  // update mouseover target
  EntityId newMouseOverTarget = NullEntityId;
  m_stickyTargetingTimer.tick(dt);
  // The cursor mouseover scan is a spatial entity query plus a lightLevel
  // evaluation per candidate, every frame, almost always finding nothing.
  // Scanning every 3rd frame just means a monster health bar appears up to
  // ~100ms after the cursor touches the monster.
  static uint64_t s_mouseoverScanCounter = 0;
  List<DamageBarEntityPtr> mouseoverEntities;
  if (++s_mouseoverScanCounter % 3 == 0) {
    mouseoverEntities = m_client->worldClient()->query<DamageBarEntity>(RectF::withCenter(cursorWorldPos, Vec2F(1, 1)), [=](shared_ptr<DamageBarEntity> const& entity) {
        return entity != player
          && entity->damageBar() == DamageBarType::Default
          && (entity->getTeam().type == TeamType::Enemy || entity->getTeam().type == TeamType::PVP)
          && m_client->worldClient()->lightLevel(entity->position()) > 0;
      });
    sortByComputedValue(mouseoverEntities, [&](DamageBarEntityPtr const& a) {
        return m_client->worldClient()->geometry().diff(a->position(), cursorWorldPos).magnitude();
      });
  } else if (m_lastMouseoverTarget != NullEntityId) {
    // Keep the current target alive between scans so an active health bar
    // doesn't flicker.
    if (auto current = as<DamageBarEntity>(m_client->worldClient()->entity(m_lastMouseoverTarget)))
      mouseoverEntities.append(std::move(current));
  }
  if (mouseoverEntities.size() > 0) {
    newMouseOverTarget = mouseoverEntities[0]->entityId();
  } else if (m_lastMouseoverTarget == NullEntityId && player->lastDamagedTarget() != NullEntityId && player->timeSinceLastGaveDamage() < m_stickyTargetingTimer.time / 2) {
    if (auto targetEntity = as<DamageBarEntity>(m_client->worldClient()->entity(player->lastDamagedTarget()))) {
      if (targetEntity->damageBar() == DamageBarType::Default && (targetEntity->getTeam().type == TeamType::Enemy || targetEntity->getTeam().type == TeamType::PVP)) {
        newMouseOverTarget = targetEntity->entityId();
      }
    }
  }
  if (newMouseOverTarget != NullEntityId && newMouseOverTarget != m_lastMouseoverTarget) {
    m_lastMouseoverTarget = newMouseOverTarget;
    m_portraitScale = 0;
    m_stickyTargetingTimer.reset();
  }

  if (m_stickyTargetingTimer.ready())
    m_lastMouseoverTarget = NullEntityId;

  // special damage bar entities
  // Fixed content config -- resolve once instead of an assets-cache query
  // (mutex + path parse) every single frame.
  static Json const barConfig = Root::singleton().assets()->json("/interface.config:specialDamageBar");
  static size_t const maxBars = barConfig.getUInt("maxCount",10);

  for (auto it = m_specialDamageBars.begin(); it != m_specialDamageBars.end();) {
    auto& bar = *it;
    auto damageBarEntity = as<DamageBarEntity>(m_client->worldClient()->entity(bar.first));
    if (damageBarEntity && damageBarEntity->damageBar() == DamageBarType::Special) {
      float targetHealth = damageBarEntity->health() / damageBarEntity->maxHealth();
      float fillSpeed = 1.0f / barConfig.getFloat("fillTime");
      if (abs(targetHealth - bar.second) < fillSpeed * dt)
        bar.second = targetHealth;
      else
        bar.second += copysign(1.0f, targetHealth - bar.second) * fillSpeed * dt;

      it++;
    } else {
      it = m_specialDamageBars.erase(it);
    }
  }

  // Scanning EVERY loaded entity for special (boss) damage bars each frame
  // is O(entities) of pure overhead in the overwhelmingly common case of no
  // boss present; every 8th frame is more than fast enough for a bar that
  // slides in when a boss appears.
  if (++m_specialBarScanCounter % 8 == 0 && m_specialDamageBars.size() < maxBars && m_client->mainPlayer()->inWorld()) {
    List<DamageBarEntityPtr> specialDamageTargets;
    m_client->worldClient()->forAllEntities([&specialDamageTargets](EntityPtr const& entity) {
        if (auto damageBarEntity = as<DamageBarEntity>(entity))
          if (damageBarEntity->damageBar() == DamageBarType::Special)
            specialDamageTargets.append(damageBarEntity);
      });
    sortByComputedValue(specialDamageTargets, [&](DamageBarEntityPtr entity) {
        return m_client->worldClient()->geometry().diff(entity->position(), m_client->mainPlayer()->position());
      });

    for (size_t i = 0; i < specialDamageTargets.size(); i++) {
      auto id = specialDamageTargets[i]->entityId();
      if (!m_specialDamageBars.contains(id)) {
        m_specialDamageBars.add(id,0);
        if (m_specialDamageBars.size() >= maxBars)
          break;
      }
    }
  }

  for (auto const& message : m_client->mainPlayer()->pullQueuedMessages())
    queueMessage(message);

  auto chatHeight = (m_chat->active() && m_chat->visible() > 0.1) ? m_chat->size()[1] : 0;
  m_radioMessagePopup->setChatHeight(chatHeight);
  if (!m_cinematicOverlay || m_cinematicOverlay->completed()) {
    // A radio interrupt is meant to be a deliberate, one-shot dismissal. Some
    // input paths (notably gamepad input feeding scripts that request an
    // interrupt) can request it every single frame, which would dismiss every
    // message the instant it appears. Debounce so that only the first request
    // after a quiet gap is honored; a continuous stream is treated as one.
    m_timeSinceRadioInterruptRequest += dt;
    if (m_client->mainPlayer()->interruptRadioMessage()) {
      constexpr float RadioInterruptDebounceTime = 0.5f;
      if (m_timeSinceRadioInterruptRequest >= RadioInterruptDebounceTime) {
        Logger::info("RadioPopup: interrupt honored ({}s since previous request)", m_timeSinceRadioInterruptRequest);
        m_radioMessagePopup->interrupt();
      }
      m_timeSinceRadioInterruptRequest = 0.0f;
    }
    if (!m_radioMessagePopup->messageActive()) {
      if (auto radioMessage = m_client->mainPlayer()->pullPendingRadioMessage()) {
        Logger::info("RadioPopup: pulled pending message '{}' (popup idle)", radioMessage->messageId);
        m_radioMessagePopup->setMessage(*radioMessage);
        m_paneManager.displayRegisteredPane(MainInterfacePanes::RadioMessagePopup);
        ChatReceivedMessage message = {
          {MessageContext::RadioMessage},
          ServerConnectionId,
          radioMessage->senderName,
          radioMessage->text,
          Text::stripEscapeCodes(radioMessage->portraitImage.replace("<frame>", "0"))
        };
        m_chat->addMessages({message}, false);
      } else {
        m_paneManager.dismissRegisteredPane(MainInterfacePanes::RadioMessagePopup);
      }
    }

    m_client->mainPlayer()->setInCinematic(false);
  } else {
    m_client->mainPlayer()->setInCinematic(true);
  }

  for (auto const& drop : m_client->mainPlayer()->pullQueuedItemDrops())
    queueItemPickupText(drop);

  m_chat->addMessages(m_client->pullChatMessages());

  if (auto worldClient = m_client->worldClient()) {
    if (worldClient->inWorld()) {
      if (auto cinematic = m_client->mainPlayer()->pullPendingCinematic()) {
        if (*cinematic)
          m_cinematicOverlay->load(Root::singleton().assets()->fetchJson(cinematic.take()));
        else
          m_cinematicOverlay->stop();
      }
    }
  }

  if (!m_confirmationDialog->isDisplayed()) {
    if (auto confirmation = m_client->mainPlayer()->pullPendingConfirmation()) {
      m_paneManager.displayRegisteredPane(MainInterfacePanes::Confirmation);
      m_confirmationDialog->displayConfirmation(confirmation->first, confirmation->second);
    }
  } else {
    auto confirmationSource = m_confirmationDialog->sourceEntityId();
    if (confirmationSource && !m_client->worldClient()->playerCanReachEntity(*confirmationSource))
      m_confirmationDialog->dismiss();
  }

  if (!m_joinRequestDialog->isDisplayed()) {
    if (auto req = m_queuedJoinRequests.maybeTakeLast()) {
      m_paneManager.displayRegisteredPane(MainInterfacePanes::JoinRequest);
      m_joinRequestDialog->displayRequest(req->first, [req](P2PJoinRequestReply reply) mutable {
          req->second.fulfill(reply);
        });
    }
  }

  for (EntityId id : m_interactionScriptPanes.keys()) {
    if (!m_paneManager.isDisplayed(m_interactionScriptPanes[id]))
      m_interactionScriptPanes.remove(id);
  }

  if (!m_messages.contains(m_overflowMessage))
    m_messageOverflow = 0;
  unsigned maxMessages = m_messageOverflow == 0 ? m_config->maxMessageCount : m_config->maxMessageCount + 1; // exclude overflow message
  if (m_messages.size() > maxMessages) {
    if (m_messageOverflow == 0) {
      m_messages.prepend(m_overflowMessage);
    }

    m_messageOverflow++;
    m_overflowMessage->message = m_config->overflowMessageText.replace("<count>", toString(m_messageOverflow));
    m_overflowMessage->cooldown = m_config->messageTime;
    if (auto oldest = m_messages.sorted([](GuiMessagePtr a, GuiMessagePtr b) { return a->cooldown < b->cooldown; }).maybeFirst())
      m_overflowMessage->cooldown = oldest.value()->cooldown;

    if (auto bottom = m_messages.filtered([this](GuiMessagePtr m) { return m != m_overflowMessage; }).maybeFirst())
      bottom.value()->cooldown = 0;
  }

  for (auto it = m_messages.begin(); it != m_messages.end();) {
    auto& message = *it;
    message->cooldown -= dt;
    if (message->cooldown < 0)
      it = m_messages.erase(it);
    else
      it++;
  }

  for (auto it = m_itemDropMessages.begin(); it != m_itemDropMessages.end();) {
    auto& message = *it;
    if (message.second.second->cooldown < 0)
      it = m_itemDropMessages.erase(it);
    else
      it++;
  }

  bool playerInWorld = m_client->mainPlayer()->inWorld();
  if (m_cinematicOverlay->completed()) {
    if (m_planetNameTimer.tick(dt)) {
      m_paneManager.dismissRegisteredPane(MainInterfacePanes::PlanetText);
    } else {
      if (playerInWorld) {
        String worldName;
        if (auto worldTemplate = m_client->worldClient()->currentTemplate())
          worldName = worldTemplate->worldName();

        if (!worldName.empty()) {
          m_planetText->parent()->setPosition(m_config->planetNameOffset * (2.f / interfaceScale()));
          m_planetText->setText(strf(m_config->planetNameFormatString.utf8Ptr(), worldName));
          m_paneManager.displayRegisteredPane(MainInterfacePanes::PlanetText);
        }
      }

      Color textColor = Color::White; // probably need to make this jsonable
      float fadeTimer = m_planetNameTimer.timer;
      if (fadeTimer < m_config->planetNameFadeTime)
        textColor.setAlphaF(fadeTimer / m_config->planetNameFadeTime);

      m_planetText->setColor(textColor);
    }
  } else if (!playerInWorld) {
    m_planetNameTimer.reset();
    m_paneManager.dismissRegisteredPane(MainInterfacePanes::PlanetText);
  }

  for (auto& containerResult : m_containerInteractor->pullContainerResults()) {
    if (!m_containerPane || !m_containerPane->giveContainerResult(containerResult)) {
      if (!m_inventoryWindow->giveContainerResult(containerResult)) {
        for (auto item : containerResult) {
          if (m_containerInteractor->containerOpen())
            m_containerInteractor->addToContainer(m_client->mainPlayer()->pickupItems(item));
          else
            m_client->mainPlayer()->giveItem(item);
        }
      }
    }
  }

  if (auto currentQuest = m_client->questManager()->currentQuest()) {
    m_paneManager.displayRegisteredPane(MainInterfacePanes::QuestTracker);
    m_questTracker->setQuest(*currentQuest);
  } else {
    m_paneManager.dismissRegisteredPane(MainInterfacePanes::QuestTracker);
  }

  updateCursor();

#ifdef STAR_SYSTEM_SWITCH
  // Nameplate/quest-indicator refresh is a spatial world query plus spring
  // stepping per frame; under load run it every 2nd frame with accumulated
  // dt (spring positions stay wall-clock correct, at half granularity).
  {
    int64_t nowUs = Time::monotonicMicroseconds();
    bool miUnderLoad = m_lastPainterUpdateTimeUs != 0 && (nowUs - m_lastPainterUpdateTimeUs) > 50000;
    m_lastPainterUpdateTimeUs = nowUs;
    m_pendingPainterDt += dt;
    if (!miUnderLoad || (++m_painterUpdateCounter % 2 == 0)) {
      m_nameplatePainter->update(m_pendingPainterDt, m_client->worldClient(), m_worldPainter->camera(), m_client->worldClient()->interactiveHighlightMode());
      m_questIndicatorPainter->update(m_pendingPainterDt, m_client->worldClient(), m_worldPainter->camera());
      m_pendingPainterDt = 0.0f;
    }
  }
#else
  m_nameplatePainter->update(dt, m_client->worldClient(), m_worldPainter->camera(), m_client->worldClient()->interactiveHighlightMode());
  m_questIndicatorPainter->update(dt, m_client->worldClient(), m_worldPainter->camera());
#endif

  m_chatBubbleManager->setCamera(m_worldPainter->camera());
  if (auto worldClient = m_client->worldClient()) {
    auto chatActions = worldClient->pullPendingChatActions();

    for (auto& action : chatActions) {
      if (action.is<PortraitChatAction>()) {
        PortraitChatAction& portraitAction = action.get<PortraitChatAction>();

        String name;
        if (auto npc = as<Npc>(worldClient->entity(portraitAction.entity)))
          name = npc->name();

        ChatReceivedMessage message = {
          {MessageContext::World},
          ServerConnectionId,
          Text::stripEscapeCodes(name),
          Text::stripEscapeCodes(portraitAction.text),
          Text::stripEscapeCodes(portraitAction.portrait.replace("<frame>", "0"))};
        m_chat->addMessages({message}, false);
      } else if (action.is<SayChatAction>()) {
        SayChatAction& sayAction = action.get<SayChatAction>();

        if (sayAction.config) {
          if (auto message = sayAction.config.opt("message"))
            m_chat->addMessages({ChatReceivedMessage(*message)}, sayAction.config.getBool("showPane", false));
        }
      }
    }

    m_chatBubbleManager->addChatActions(chatActions);
    m_chatBubbleManager->update(dt, worldClient);
  }

  if (auto container = m_client->worldClient()->get<ContainerEntity>(m_containerInteractor->openContainerId())) {
    if (!m_client->worldClient()->playerCanReachEntity(container->entityId())
        || !container->isInteractive())
      m_containerInteractor->closeContainer();
  }

  for (auto& pair : m_canvases) {
    pair.second->setPosition(Vec2I());
    if (pair.second->ignoreInterfaceScale())
      pair.second->setSize(Vec2I(m_guiContext->windowSize()));
    else
      pair.second->setSize(Vec2I(m_guiContext->windowInterfaceSize()));
    pair.second->update(dt);
  }
}

void MainInterface::renderInWorldElements() {
  if (m_disableHud)
    return;

  m_guiContext->clearTextStyle();
  m_questIndicatorPainter->render();
  m_nameplatePainter->render();
  m_chatBubbleManager->render();
}

bool MainInterface::hudSafeToReplay() const {
  return !m_disableHud
      && !m_actionWheelActive
      && !m_cursorDraggingCached
      && !m_clientCommandProcessor->debugDisplayEnabled()
      && m_paneManager.displayedPanesVersion() == m_hudRenderedPanesVersion;
}

void MainInterface::setHudForceReplay(bool force) {
  m_hudForceReplay = force;
  m_paneManager.setForceReplay(force);
}

void MainInterface::setCursorDrawnSeparately(bool separate) {
  m_cursorDrawnSeparately = separate;
}

void MainInterface::renderCursorOverlay() {
  if (m_disableHud)
    return;
  renderCursor();
  m_guiContext->clearTextStyle();
}

void MainInterface::render() {
  if (m_disableHud)
    return;

#ifdef STAR_SYSTEM_SWITCH
  // HUD-phase breakdown ([perf-mi]), logged every ~150 frames.
  static int64_t s_miFrames = 0, s_miBars = 0, s_miCanvases = 0, s_miWindows = 0, s_miRest = 0;
  int64_t miLapTime = Time::monotonicMicroseconds();
  auto miLap = [&miLapTime](int64_t& acc) {
    int64_t n = Time::monotonicMicroseconds();
    acc += n - miLapTime;
    miLapTime = n;
  };
#endif
  m_guiContext->clearTextStyle();
  // Same under-load replay pattern as PaneManager's pane cache: the HUD bar
  // group is an immediate-mode rebuild costing ~4-5ms/frame; when this run is
  // measurably struggling (< ~20fps) render it fresh only every 3rd frame and
  // replay the recorded primitives in between.
  ++m_barsRenderFrame;
  {
    int64_t nowUs = Time::monotonicMicroseconds();
    int64_t gapUs = m_lastBarsRenderTimeUs != 0 ? nowUs - m_lastBarsRenderTimeUs : 0;
    bool underLoad = gapUs > 50000;
    int64_t replayInterval = gapUs > 75000 ? 4 : 3;
    m_lastBarsRenderTimeUs = nowUs;
    auto const& renderer = m_guiContext->renderer();
    bool replayed = false;
    if (!m_barsRecording.empty()
        && (m_hudForceReplay || (underLoad && m_barsRenderFrame - m_barsRecordingFrame < replayInterval))) {
      renderer->playPrimitiveRecording(m_barsRecording);
      replayed = true;
    }
    if (!replayed) {
      bool recording = underLoad && renderer->beginPrimitiveRecording();
      renderBreath();
      renderMessages();
      renderMonsterHealthBar();
      renderSpecialDamageBar();
      renderMainBar();
      // Standalone canvases are recorded with the bar group: canvas draw ops
      // are written by Lua scripts that execute inside the sim tick, so the
      // pure-replay frames (which may overlap that tick) must not iterate
      // them -- they replay the recording made here instead.
      RectI canvasScreenRect = RectI::withSize(Vec2I(), Vec2I(m_guiContext->windowSize()));
      for (auto& pair : m_canvases)
        pair.second->render(canvasScreenRect);
      if (recording) {
        m_barsRecording = renderer->endPrimitiveRecording();
        m_barsRecordingFrame = m_barsRenderFrame;
      }
    }
  }
#ifdef STAR_SYSTEM_SWITCH
  miLap(s_miBars);
#endif
  renderDebug();

#ifdef STAR_SYSTEM_SWITCH
  miLap(s_miCanvases);
#endif
  renderWindows();
#ifdef STAR_SYSTEM_SWITCH
  miLap(s_miWindows);
#endif
  renderActionWheel();
  if (!m_cursorDrawnSeparately)
    renderCursor();
  m_hudRenderedPanesVersion = m_paneManager.displayedPanesVersion();
#ifdef STAR_SYSTEM_SWITCH
  miLap(s_miRest);
  if (++s_miFrames >= 150) {
    Logger::info("[perf-mi] bars={:.1f}ms canvases={:.1f} windows={:.1f} rest={:.1f}",
        s_miBars / 1e3 / s_miFrames, s_miCanvases / 1e3 / s_miFrames,
        s_miWindows / 1e3 / s_miFrames, s_miRest / 1e3 / s_miFrames);
    s_miFrames = 0;
    s_miBars = s_miCanvases = s_miWindows = s_miRest = 0;
  }
#endif
}

Vec2F MainInterface::cursorWorldPosition() const {
  return m_worldPainter->camera().screenToWorld(m_cursorScreenPos);
}

bool MainInterface::isDebugDisplayed() {
  return m_clientCommandProcessor->debugDisplayEnabled();
}

void MainInterface::doChat(String const& chat, bool addToHistory) {
  if (chat.empty())
    return;

  if (chat.beginsWith("/")) {
    m_lastCommand = chat;

    for (auto const& result : m_clientCommandProcessor->handleCommand(chat, true))
      m_chat->addLine(result);
  } else {
    m_client->sendChat(chat, m_chat->sendMode());
  }

  if (addToHistory)
    m_chat->addHistory(chat);
}

void MainInterface::queueMessage(String const& message, Maybe<float> cooldown, float spring) {
  auto guiMessage = make_shared<GuiMessage>(message, cooldown.value(m_config->messageTime), spring);
  m_messages.append(guiMessage);
}

void MainInterface::queueMessage(String const& message) {
  queueMessage(message, m_config->messageTime, 0.0f);
}

void MainInterface::queueJoinRequest(pair<String, RpcPromiseKeeper<P2PJoinRequestReply>> request)
{
  m_queuedJoinRequests.push_back(request);
}

void MainInterface::queueItemPickupText(ItemPtr const& item) {
  auto descriptor = item->descriptor();
  if (m_itemDropMessages.contains(descriptor.singular())) {
    auto countMessPair = m_itemDropMessages.get(descriptor.singular());
    auto newCount = item->count() + countMessPair.first;
    auto message = countMessPair.second;
    message->message = strf("{} - {}", item->friendlyName(), newCount);
    message->cooldown = m_config->messageTime;
    m_itemDropMessages[descriptor.singular()] = {newCount, message};
  } else {
    auto message = make_shared<GuiMessage>(strf("{} - {}", item->friendlyName(), item->count()), m_config->messageTime);
    m_messages.append(message);
    m_itemDropMessages[descriptor.singular()] = {item->count(), message};
  }
}

bool MainInterface::fixedCamera() const {
  return m_clientCommandProcessor->fixedCameraEnabled();
}

bool MainInterface::hudVisible() const {
  return !m_disableHud;
}

void MainInterface::setHudVisible(bool visible) {
  m_disableHud = !visible;
}

void MainInterface::warpToOrbitedWorld(bool deploy) {
  if (m_client->canBeamDown(deploy)) {
    if (deploy)
      m_client->warpPlayer(WarpAlias::OrbitedWorld, true, "deploy", true);
    else
      m_client->warpPlayer(WarpAlias::OrbitedWorld, true, "beam");
    return;
  }
  m_guiContext->playAudio("/sfx/interface/clickon_error.ogg");
}

void MainInterface::warpToOwnShip() {
  if (m_client->canBeamUp()) {
    warpTo(WarpAlias::OwnShip);
  } else {
    m_guiContext->playAudio("/sfx/interface/clickon_error.ogg");
  }
}

void MainInterface::warpTo(WarpAction const& warpAction) {
  if (m_client->beamUpRule() == BeamUpRule::AnywhereWithWarning) {
    if (m_confirmationDialog->isDisplayed())
      m_confirmationDialog->dismiss();

    m_paneManager.displayRegisteredPane(MainInterfacePanes::Confirmation);
    m_confirmationDialog->displayConfirmation("/interface/windowconfig/beamupconfirmation.config", [this, warpAction] (Widget*) {
        m_client->warpPlayer(warpAction, true, "beam");
      }, [](Widget*) {});
  } else {
    m_client->warpPlayer(warpAction, true, "beam");
  }
}

CanvasWidgetPtr MainInterface::fetchCanvas(String const& canvasName, bool ignoreInterfaceScale) {
  CanvasWidgetPtr canvas;

  if (auto canvasPtr = m_canvases.ptr(canvasName))
    canvas = *canvasPtr;
  else {
    m_canvases.emplace(canvasName, canvas = make_shared<CanvasWidget>());
    canvas->setPosition(Vec2I());
    if (ignoreInterfaceScale)
      canvas->setSize(Vec2I(m_guiContext->windowSize()));
    else
      canvas->setSize(Vec2I(m_guiContext->windowInterfaceSize()));
  }

  canvas->setIgnoreInterfaceScale(ignoreInterfaceScale);
  return canvas;
}

ClientCommandProcessorPtr MainInterface::commandProcessor() const {
  return m_clientCommandProcessor;
}

// For when the player swaps characters. We need to completely reload ScriptPanes,
// because a lot of ScriptPanes do not expect the character to suddenly change and may break or spill data over.
void MainInterface::takeScriptPanes(List<ScriptPaneInfo>& out) {
  m_paneManager.dismissWhere([&](PanePtr const& pane) {
    if (auto scriptPane = as<ScriptPane>(pane)) {
      if (scriptPane->isDismissed())
        return false;
      auto sourceEntityId = scriptPane->sourceEntityId();
      m_interactionScriptPanes.remove(sourceEntityId);
      auto& info = out.emplaceAppend();
      info.scriptPane = scriptPane;
      info.config = scriptPane->rawConfig();
      info.sourceEntityId = sourceEntityId;
      info.visible = scriptPane->visibility();
      info.position = scriptPane->relativePosition();

      return true;
    }
    return false;
  });
}

void MainInterface::reviveScriptPanes(List<ScriptPaneInfo>& panes) {
  for (auto& info : panes) { // this is evil and stupid
    info.scriptPane->~ScriptPane();
    new(info.scriptPane.get()) ScriptPane(m_client, info.config, info.sourceEntityId);
    info.scriptPane->setVisibility(info.visible);
    displayScriptPane(info.scriptPane, info.sourceEntityId);
    info.scriptPane->setPosition(info.position);
  }
}

void MainInterface::displayDefaultPanes() {
  m_paneManager.displayRegisteredPane(MainInterfacePanes::ActionBar);
  m_paneManager.displayRegisteredPane(MainInterfacePanes::Chat);
  m_paneManager.displayRegisteredPane(MainInterfacePanes::TeamBar);
  m_paneManager.displayRegisteredPane(MainInterfacePanes::StatusPane);
}

PanePtr MainInterface::createEscapeDialog() {
  auto assets = Root::singleton().assets();

  auto escapeDialog = make_shared<Pane>();
  auto escapeDialogPtr = escapeDialog.get();

  GuiReader escapeDialogReader;
  escapeDialogReader.registerCallback("returnToGame", [escapeDialogPtr](Widget*) {
      escapeDialogPtr->dismiss();
    });
  escapeDialogReader.registerCallback("showOptions", [escapeDialogPtr, this](Widget*) {
      escapeDialogPtr->dismiss();
      m_paneManager.displayRegisteredPane(MainInterfacePanes::Options);
    });
  escapeDialogReader.registerCallback("saveAndQuit", [escapeDialogPtr, this](Widget*) {
      m_state = ReturnToTitle;
      escapeDialogPtr->dismiss();
    });

  escapeDialogReader.construct(assets->json("/interface.config:escapeDialog"), escapeDialogPtr);
  escapeDialog->fetchChild<LabelWidget>("lblversion")->setText(strf("OpenStarbound - {} ({})", StarVersionString, StarArchitectureString));
  return escapeDialog;
}

float MainInterface::interfaceScale() const {
  return m_guiContext->interfaceScale();
}

unsigned MainInterface::windowHeight() const {
  return m_guiContext->windowHeight();
}

unsigned MainInterface::windowWidth() const {
  return m_guiContext->windowWidth();
}

Vec2F MainInterface::mainBarPosition() const {
  return Vec2F(windowWidth(), windowHeight()) - Vec2F(m_config->mainBarSize) * interfaceScale();
}

void MainInterface::renderBreath() {
  auto assets = Root::singleton().assets();
  auto imgMetadata = Root::singleton().imageMetadataDatabase();

  Vec2I breathBarSize = Vec2I(m_guiContext->textureSize("/interface/breath/empty.png")) * interfaceScale();
  Vec2I breathOffset = jsonToVec2I(assets->json("/interface.config:breathPos"));

  Vec2F breathBackgroundCenterPos(windowWidth() * 0.5f + breathOffset[0] * interfaceScale(), windowHeight() - breathOffset[1] * interfaceScale());
  Vec2F breathBarPos = breathBackgroundCenterPos + Vec2F(jsonToVec2I(assets->json("/interface.config:breathBarPos")) * interfaceScale());

  float breath = m_client->mainPlayer()->breath();
  float breathMax = m_client->mainPlayer()->maxBreath();

  size_t blocks = round((10 * breath) / breathMax);

  if (blocks < 10) {
    String breathPath = "/interface/breath/breath.png";
    m_guiContext->drawQuad(breathPath, RectF::withCenter(breathBackgroundCenterPos, Vec2F(imgMetadata->imageSize(breathPath)) * interfaceScale()));
    for (size_t i = 0; i < 10; i++) {
      if (i >= blocks) {
        if (blocks == 0 && Time::monotonicMilliseconds() % 500 > 250)
          m_guiContext->drawQuad("/interface/breath/warning.png", breathBarPos + Vec2F(breathBarSize[0] * i, 0), interfaceScale());
        else
          m_guiContext->drawQuad("/interface/breath/empty.png", breathBarPos + Vec2F(breathBarSize[0] * i, 0), interfaceScale());
      } else {
        m_guiContext->drawQuad("/interface/breath/breathbar.png", breathBarPos + Vec2F(breathBarSize[0] * i, 0), interfaceScale());
      }
    }
  }
}

void MainInterface::renderMessages() {
  if (m_messages.empty())
    return;

  Vec2F totalOffset = {};
  auto imgMetadata = Root::singleton().imageMetadataDatabase();
  unsigned bottomOffset = Root::singleton().configuration()->getPath("inventory.bottomActionBar").optBool().value(false) ? 32 : 0;
  for (auto& message : m_messages) {
    Vec2F hiddenOffset = Vec2F(m_config->messageHiddenOffset);
    Vec2F activeOffset = Vec2F(m_config->messageActiveOffset);
    if (bottomOffset) {
      activeOffset[1] += bottomOffset;
      bottomOffset = 0;
    }
    Vec2F messageOffset = lerp(message->springState, Vec2F(), activeOffset - hiddenOffset);
    totalOffset += messageOffset;
    messageOffset = totalOffset + hiddenOffset;

    Vec2F backgroundCenterPos = Vec2F(windowWidth() * 0.5f + messageOffset[0] * interfaceScale(), messageOffset[1] * interfaceScale());

    Vec2F backgroundTextCenterPos = backgroundCenterPos + Vec2F(m_config->messageTextContainerOffset) * interfaceScale();
    Vec2F messageTextOffset = backgroundTextCenterPos + Vec2F(m_config->messageTextOffset) * interfaceScale();

    if (message->cooldown > m_config->messageHideTime)
      message->springState = (message->springState * m_config->messageWindowSpring + 1.0f) / (m_config->messageWindowSpring + 1.0f);
    else
      message->springState = (message->springState * m_config->messageWindowSpring) / (m_config->messageWindowSpring + 1.0f);

    m_guiContext->drawQuad(m_config->messageTextContainer,
        RectF::withCenter(backgroundTextCenterPos, Vec2F(imgMetadata->imageSize(m_config->messageTextContainer)) * interfaceScale()));

    m_guiContext->setTextStyle(m_config->textStyle);
    m_guiContext->renderText(message->message, {messageTextOffset, HorizontalAnchor::HMidAnchor, VerticalAnchor::VMidAnchor});
  }
}

void MainInterface::renderMonsterHealthBar() {
  auto assets = Root::singleton().assets();
  auto imgMetadata = Root::singleton().imageMetadataDatabase();
  if (m_lastMouseoverTarget != NullEntityId && !m_stickyTargetingTimer.ready()) {
    auto world = m_client->worldClient();

    auto entity = world->entity(m_lastMouseoverTarget);
    auto showDamageEntity = as<DamageBarEntity>(entity);

    if (!showDamageEntity) {
      m_lastMouseoverTarget = NullEntityId;
      return;
    }

    Vec2F backgroundCenterPos = Vec2F(windowWidth() / 2.0f, windowHeight());

    auto container = assets->json("/interface.config:monsterHealth.container").toString();
    auto offset = jsonToVec2F(assets->json("/interface.config:monsterHealth.offset")) * interfaceScale();
    m_guiContext->drawQuad(container, RectF::withCenter(backgroundCenterPos + offset, Vec2F(imgMetadata->imageSize(container) * interfaceScale())));

    auto nameTextOffset = jsonToVec2F(assets->json("/interface.config:monsterHealth.nameTextOffset")) * interfaceScale();
    m_guiContext->setTextStyle(m_config->textStyle);
    m_guiContext->renderText(showDamageEntity->name(), backgroundCenterPos + nameTextOffset);

    auto empty = assets->json("/interface.config:monsterHealth.progressEmpty").toString();
    auto filled = assets->json("/interface.config:monsterHealth.progressFilled").toString();
    auto progressBarOffset = jsonToVec2F(assets->json("/interface.config:monsterHealth.progressBarOffset")) * interfaceScale();
    auto chunks = assets->json("/interface.config:monsterHealth.progressChunks").toInt();
    int blocks = round(showDamageEntity->health() / showDamageEntity->maxHealth() * chunks);
    Vec2F barPos = backgroundCenterPos + progressBarOffset;
    Vec2F barItemOffset = Vec2F(imgMetadata->imageSize(filled)) * interfaceScale();
    barItemOffset[1] = 0;

    m_guiContext->drawQuad(empty, RectF::withSize(backgroundCenterPos + barPos, Vec2F(imgMetadata->imageSize(empty)) * interfaceScale()));

    for (int i = 0; i < blocks; i++)
      m_guiContext->drawQuad(filled, barPos + barItemOffset * i, interfaceScale());

    auto portraitOffset = jsonToVec2F(assets->json("/interface.config:monsterHealth.portraitOffset")) * interfaceScale();
    auto portraitScale = assets->json("/interface.config:monsterHealth.portraitScale").toFloat() * interfaceScale();

    auto portraitScissorRect = jsonToRectF(assets->json("/interface.config:monsterHealth.portraitScissorRect")).scaled(interfaceScale());
    auto rect = portraitScissorRect.translated(backgroundCenterPos + portraitOffset);
    m_guiContext->setInterfaceScissorRect(RectI(RectF(rect).scaled(1.0f / interfaceScale())));
    auto portraitMaxSize = jsonToVec2I(assets->json("/interface.config:monsterHealth.portraitMaxSize"));
    List<Drawable> portrait = showDamageEntity->portrait(PortraitMode::Full);

    auto bounds = Drawable::boundBoxAll(portrait, true);
    if (m_portraitScale == 0)
      m_portraitScale = max<int>(1, ceil(max(bounds.size().x() / portraitMaxSize.x(), bounds.size().y() / portraitMaxSize.y())));
    Drawable::translateAll(portrait, {-bounds.xMin() - (bounds.width() * 0.5f), -bounds.yMin() }); // crop out whitespace, align bottom center
    Drawable::scaleAll(portrait, 1.0f / m_portraitScale);

    for (auto drawable : portrait)
      m_guiContext->drawDrawable(std::move(drawable), backgroundCenterPos + portraitOffset, portraitScale);

    m_guiContext->resetInterfaceScissorRect();
  }
}

void MainInterface::renderSpecialDamageBar() {
  // NOTE: num and i including EVERYTHING will leave empty spaces for nonexistent entities in the list. Shouldn't persist for more than a frame.
  size_t num = m_specialDamageBars.size();
  if (num == 0) return;

  auto assets = Root::singleton().assets();
  auto imgMetadata = Root::singleton().imageMetadataDatabase();

  auto barConfig = assets->json("/interface.config:specialDamageBar");

  // hSpacing should be equivalent to the width of the full bar, plus a bit of spacing
  float hSpacing = barConfig.getFloat("multibarSpacing",jsonToVec2F(barConfig.get("backgroundOffset")).x()*-2.0f) * interfaceScale();

  size_t i = 0;
  float totalWidth = hSpacing*num;
  float maxWidth = std::min(totalWidth,windowWidth()-(barConfig.getFloat("multibarScreenEdgeOffset",20.0f)*interfaceScale()));
  float hScale = maxWidth/totalWidth;//1.0f/num;

  float allOffset = -hSpacing*0.5f*(num-1);
  float center = windowWidth()/2.0f;

  auto background = barConfig.getString("background");
  auto backgroundOffset = jsonToVec2F(barConfig.get("backgroundOffset")) * interfaceScale();
  backgroundOffset.setX(backgroundOffset.x()*hScale);

  auto backgroundImageSize = Vec2F(imgMetadata->imageSize(background)) * interfaceScale();
  backgroundImageSize.setX(backgroundImageSize.x()*hScale);

  auto fill = barConfig.getString("fill");
  auto fillOffset = jsonToVec2F(barConfig.get("fillOffset")) * interfaceScale();
  fillOffset.setX(fillOffset.x()*hScale);

  auto nameOffset = jsonToVec2F(barConfig.get("nameOffset")) * interfaceScale();
  nameOffset.setX(nameOffset.x()*hScale);

  for (auto& bar : m_specialDamageBars) {
      if (auto target = as<DamageBarEntity>(m_client->worldClient()->entity(bar.first))) {

        Vec2F bottomCenter = Vec2F(center + (allOffset + hSpacing*i)*hScale, 0);

        auto screenPos = RectF::withSize(bottomCenter + backgroundOffset, backgroundImageSize);
        m_guiContext->drawQuad(background, screenPos);

        Vec2F size = Vec2F(barConfig.getInt("fillWidth") * bar.second, imgMetadata->imageSize(fill).y());
        size.setX(size.x()*hScale);

        m_guiContext->drawQuad(fill, RectF::withSize(bottomCenter + fillOffset, size * interfaceScale()));

        m_guiContext->setFontColor(jsonToColor(barConfig.get("nameColor")).toRgba());
        m_guiContext->setFontSize(barConfig.getUInt("nameSize"));
        if (auto style = barConfig.get("nameStyle"))
          m_guiContext->setTextStyle(style);
        m_guiContext->renderText(target->name(), TextPositioning(bottomCenter + nameOffset, HorizontalAnchor::HMidAnchor, VerticalAnchor::BottomAnchor));
        m_guiContext->clearTextStyle();
    }
    i++;
  }
}

void MainInterface::renderMainBar() {
  Vec2F barPos = mainBarPosition();

  m_cursorTooltip = {};

  auto assets = Root::singleton().assets();

  Vec2F inventoryButtonPos = barPos + Vec2F(m_config->mainBarInventoryButtonOffset) * interfaceScale();
  if (m_paneManager.registeredPaneIsDisplayed(MainInterfacePanes::Inventory)) {
    if (overButton(m_config->mainBarInventoryButtonPoly, m_cursorScreenPos)) {
      m_guiContext->drawQuad(m_config->inventoryImageOpenHover, Vec2F(inventoryButtonPos), interfaceScale());
      m_cursorTooltip = assets->json("/interface.config:cursorTooltip.inventoryText").toString();
    } else {
      m_guiContext->drawQuad(m_config->inventoryImageOpen, Vec2F(inventoryButtonPos), interfaceScale());
    }
  } else if (overButton(m_config->mainBarInventoryButtonPoly, m_cursorScreenPos)) {
    if (m_inventoryWindow->containsNewItems())
      m_guiContext->drawQuad(m_config->inventoryImageGlowHover, Vec2F(inventoryButtonPos), interfaceScale());
    else
      m_guiContext->drawQuad(m_config->inventoryImageHover, Vec2F(inventoryButtonPos), interfaceScale());
    m_cursorTooltip = assets->json("/interface.config:cursorTooltip.inventoryText").toString();
  } else {
    if (m_inventoryWindow->containsNewItems())
      m_guiContext->drawQuad(m_config->inventoryImageGlow, Vec2F(inventoryButtonPos), interfaceScale());
    else
      m_guiContext->drawQuad(m_config->inventoryImage, Vec2F(inventoryButtonPos), interfaceScale());
  }

  auto drawStateButton = [this](MainInterfacePanes paneType, Vec2F pos, PolyI poly,
      String image, String hoverImage, String openImage, String hoverOpenImage, String toolTip) {
    if (m_paneManager.registeredPaneIsDisplayed(paneType)) {
      if (overButton(poly, m_cursorScreenPos)) {
        m_guiContext->drawQuad(hoverOpenImage, pos, interfaceScale());
        m_cursorTooltip = toolTip;
      } else {
        m_guiContext->drawQuad(openImage, pos, interfaceScale());
      }
    } else if (overButton(poly, m_cursorScreenPos)) {
      m_guiContext->drawQuad(hoverImage, pos, interfaceScale());
      m_cursorTooltip = toolTip;
    } else {
      m_guiContext->drawQuad(image, pos, interfaceScale());
    }
  };

  Vec2F craftButtonPos = barPos + Vec2F(m_config->mainBarCraftButtonOffset) * interfaceScale();
  drawStateButton(MainInterfacePanes::CraftingPlain,
      craftButtonPos,
      m_config->mainBarCraftButtonPoly,
      m_config->craftImage,
      m_config->craftImageHover,
      m_config->craftImageOpen,
      m_config->craftImageOpenHover,
      assets->json("/interface.config:cursorTooltip.craftingText").toString());

  Vec2F codexButtonPos = barPos + Vec2F(m_config->mainBarCodexButtonOffset) * interfaceScale();
  drawStateButton(MainInterfacePanes::Codex,
      codexButtonPos,
      m_config->mainBarCodexButtonPoly,
      m_config->codexImage,
      m_config->codexImageHover,
      m_config->codexImageOpen,
      m_config->codexImageHoverOpen,
      assets->json("/interface.config:cursorTooltip.codexText").toString());

  Vec2F mmUpgradeButtonPos = barPos + Vec2F(m_config->mainBarMmUpgradeButtonOffset) * interfaceScale();
  if (m_client->mainPlayer()->inventory()->essentialItem(EssentialItem::BeamAxe)) {
    drawStateButton(MainInterfacePanes::MmUpgrade,
        mmUpgradeButtonPos,
        m_config->mainBarMmUpgradeButtonPoly,
        m_config->mmUpgradeImage,
        m_config->mmUpgradeImageHover,
        m_config->mmUpgradeImageOpen,
        m_config->mmUpgradeImageHoverOpen,
        assets->json("/interface.config:cursorTooltip.mmUpgradeText").toString());
  } else {
    drawStateButton(MainInterfacePanes::MmUpgrade,
        mmUpgradeButtonPos,
        m_config->mainBarMmUpgradeButtonPoly,
        m_config->mmUpgradeImageDisabled,
        m_config->mmUpgradeImageDisabled,
        m_config->mmUpgradeImageDisabled,
        m_config->mmUpgradeImageDisabled,
        assets->json("/interface.config:cursorTooltip.disabledText").toString());
  }

  Vec2F collectionsButtonPos = barPos + Vec2F(m_config->mainBarCollectionsButtonOffset) * interfaceScale();
  drawStateButton(MainInterfacePanes::Collections,
    collectionsButtonPos,
    m_config->mainBarCollectionsButtonPoly,
    m_config->collectionsImage,
    m_config->collectionsImageHover,
    m_config->collectionsImageOpen,
    m_config->collectionsImageHoverOpen,
    assets->json("/interface.config:cursorTooltip.collectionsText").toString());

  // when the player can't deploy or beam, show the deploy button disabled
  // when the player can beam up they can't deploy down, show beaming up button in deploy button's place
  // when the player can only deploy, only show deploy button
  // when the player can deploy or beam down, show both buttons

  Vec2F deployButtonPos(Vec2F(barPos) + Vec2F(m_config->mainBarDeployButtonOffset) * interfaceScale());
  if (m_client->canBeamUp()) {
    if (overButton(m_config->mainBarDeployButtonPoly, m_cursorScreenPos)) {
      m_guiContext->drawQuad(m_config->beamUpImageHover, deployButtonPos, interfaceScale());
      m_cursorTooltip = assets->json("/interface.config:cursorTooltip.beamUpText").toString();
    } else {
      m_guiContext->drawQuad(m_config->beamUpImage, deployButtonPos, interfaceScale());
    }
  } else if (m_client->canBeamDown(true)) {
    if (overButton(m_config->mainBarDeployButtonPoly, m_cursorScreenPos)) {
      m_guiContext->drawQuad(m_config->deployImageHover, deployButtonPos, interfaceScale());
      m_cursorTooltip = assets->json("/interface.config:cursorTooltip.deployText").toString();
    } else {
      m_guiContext->drawQuad(m_config->deployImage, deployButtonPos, interfaceScale());
    }
  } else {
    m_guiContext->drawQuad(m_config->deployImageDisabled, deployButtonPos, interfaceScale());
  }

  Vec2F beamButtonPos(Vec2F(barPos) + Vec2F(m_config->mainBarBeamButtonOffset) * interfaceScale());
  if (m_client->canBeamDown()) {
    if (overButton(m_config->mainBarBeamButtonPoly, m_cursorScreenPos)) {
      m_guiContext->drawQuad(m_config->beamDownImageHover, beamButtonPos, interfaceScale());
      m_cursorTooltip = assets->json("/interface.config:cursorTooltip.beamDownText").toString();
    } else {
      m_guiContext->drawQuad(m_config->beamDownImage, beamButtonPos, interfaceScale());
    }
  }

  Vec2F questLogButtonPos = barPos + Vec2F(m_config->mainBarQuestLogButtonOffset) * interfaceScale();
  drawStateButton(MainInterfacePanes::QuestLog,
      questLogButtonPos,
      m_config->mainBarQuestLogButtonPoly,
      m_config->questLogImage,
      m_config->questLogImageHover,
      m_config->questLogImageOpen,
      m_config->questLogImageHoverOpen,
      assets->json("/interface.config:cursorTooltip.questsText").toString());
}

void MainInterface::renderActionWheel() {
  if (!m_actionWheelActive)
    return;

  auto items = activeActionWheelItems();
  if (items.empty())
    return;

  Vec2F screenSize(windowWidth(), windowHeight());
  Vec2F center = screenSize / 2.0f;
  float shortSide = std::max(1.0f, std::min(screenSize[0], screenSize[1]));
  float scale = interfaceScale();
  float outerRadius = std::clamp(shortSide * 0.18f, 68.0f * scale, 118.0f * scale);
  float innerRadius = outerRadius * 0.50f;
  float step = Constants::pi * 2.0f / items.size();
  float gap = std::min(0.035f, step * 0.18f);

  m_guiContext->drawQuad(RectF::withSize(Vec2F(), screenSize), Vec4B(0, 0, 0, 96));

  for (size_t i = 0; i < items.size(); ++i) {
    bool selected = m_actionWheelSelection && *m_actionWheelSelection == i;
    bool enabled = items[i].enabled;
    Vec4B fill = selected ? Vec4B(210, 224, 230, enabled ? 215 : 130)
        : enabled ? Vec4B(54, 60, 60, 205)
        : Vec4B(40, 40, 40, 145);
    Vec4B line = selected ? Vec4B(255, 255, 235, 245)
        : enabled ? Vec4B(144, 154, 154, 220)
        : Vec4B(95, 95, 95, 170);

    float mid = Constants::pi / 2.0f - (float)i * step;
    float start = mid - step / 2.0f + gap;
    float end = mid + step / 2.0f - gap;
    int slices = std::max(4, (int)std::ceil(std::abs(end - start) / 0.16f));
    List<tuple<Vec2F, Vec2F, Vec2F>> triangles;
    PolyF outline;

    auto point = [&](float angle, float radius) {
      return center + Vec2F(std::cos(angle), -std::sin(angle)) * radius;
    };

    Vec2F previousInner = point(start, innerRadius);
    Vec2F previousOuter = point(start, outerRadius);
    outline.add(previousOuter);
    for (int s = 1; s <= slices; ++s) {
      float angle = start + (end - start) * (float)s / slices;
      Vec2F currentInner = point(angle, innerRadius);
      Vec2F currentOuter = point(angle, outerRadius);
      triangles.append({previousInner, previousOuter, currentOuter});
      triangles.append({previousInner, currentOuter, currentInner});
      outline.add(currentOuter);
      previousInner = currentInner;
      previousOuter = currentOuter;
    }
    for (int s = slices; s >= 0; --s) {
      float angle = start + (end - start) * (float)s / slices;
      outline.add(point(angle, innerRadius));
    }

    m_guiContext->drawTriangles(triangles, fill);
    m_guiContext->drawPolyLines(outline, line, selected ? 2.2f * scale : 1.2f * scale);

    if (items[i].inventoryLocation) {
      bool pairSlot = items[i].inventoryLocation->is<CustomBarIndex>();
      Vec2F iconCenter = point(mid, (innerRadius + outerRadius) * 0.5f);
      float cellScale = std::clamp(scale * 0.92f * items[i].iconScale, 1.0f, 2.5f);
      float cellSize = 17.0f * cellScale;
      float cellGap = 1.0f * cellScale;
      Vec2F groupSize(pairSlot ? cellSize * 2.0f + cellGap : cellSize, cellSize);
      RectF groupRect = RectF::withSize(iconCenter - groupSize / 2.0f, groupSize);

      auto drawCell = [&](Vec2F const& pos, ItemPtr const& item, bool secondaryIcon) {
        RectF cellRect = RectF::withSize(pos, Vec2F::filled(cellSize));
        m_guiContext->drawQuad(cellRect, enabled ? Vec4B(22, 26, 28, 218) : Vec4B(18, 18, 18, 150));
        m_guiContext->drawPolyLines(PolyF(cellRect), Vec4B(82, 88, 88, enabled ? 235 : 150), 1.0f * scale);

        if (!item)
          return;

        String border = rarityBorder(item->rarity());
        Vec2F borderSize = Vec2F(m_guiContext->textureSize(border)) * cellScale;
        m_guiContext->drawQuad(border, cellRect.center() - borderSize / 2.0f, cellScale, enabled ? Vec4B::filled(255) : Vec4B(170, 170, 170, 155));

        List<Drawable> iconDrawables = secondaryIcon ? item->secondaryDrawables().value(item->iconDrawables()) : item->iconDrawables();
        for (auto drawable : iconDrawables)
          m_guiContext->drawDrawable(std::move(drawable), cellRect.center(), cellScale, enabled ? Vec4B::filled(255) : Vec4B(170, 170, 170, 155));

        if (item->count() > 1 && !secondaryIcon) {
          m_guiContext->setFontSize(5, scale);
          m_guiContext->setFontColor(enabled ? Vec4B(245, 245, 238, 255) : Vec4B(180, 180, 180, 160));
          m_guiContext->renderText(actionWheelShortCount(item->count()), TextPositioning(cellRect.max() + Vec2F(-1.0f, 1.0f) * scale, HorizontalAnchor::RightAnchor, VerticalAnchor::BottomAnchor));
        }
      };

      if (pairSlot) {
        drawCell(groupRect.min(), items[i].primaryItem, false);
        if (itemSafeTwoHanded(items[i].primaryItem))
          drawCell(groupRect.min() + Vec2F(cellSize + cellGap, 0.0f), items[i].primaryItem, true);
        else
          drawCell(groupRect.min() + Vec2F(cellSize + cellGap, 0.0f), items[i].secondaryItem, false);
      } else {
          drawCell(groupRect.min(), items[i].primaryItem, false);
      }

    } else if (!items[i].icon.empty()) {
      float iconScale = std::clamp(scale * 0.95f * items[i].iconScale, 1.0f, 2.8f);
      Vec2F iconSize = Vec2F(m_guiContext->textureSize(items[i].icon)) * iconScale;
      Vec2F iconPos = point(mid, (innerRadius + outerRadius) * 0.5f) - iconSize / 2.0f;
      m_guiContext->drawQuad(items[i].icon, iconPos, iconScale, enabled ? Vec4B::filled(255) : Vec4B(170, 170, 170, 155));
    }
  }

  String label = "Cancel";
  if (m_actionWheelSelection && *m_actionWheelSelection < items.size())
    label = items[*m_actionWheelSelection].label;

  m_guiContext->setTextStyle(m_config->textStyle);
  m_guiContext->setFontSize(6);
  m_guiContext->setFontColor(m_actionWheelSelection ? Vec4B(245, 245, 230, 255) : Vec4B(180, 184, 176, 230));

  RectF textBounds = m_guiContext->determineTextSize(label, TextPositioning(center, HorizontalAnchor::HMidAnchor, VerticalAnchor::VMidAnchor));
  Vec2F labelSize(
      std::max(54.0f * scale, textBounds.width() + 12.0f * scale),
      std::max(14.0f * scale, textBounds.height() + 6.0f * scale));
  RectF labelRect = RectF::withCenter(center, labelSize);
  m_guiContext->drawQuad(labelRect, Vec4B(22, 24, 24, 230));
  m_guiContext->drawPolyLines(PolyF(labelRect), Vec4B(238, 238, 226, 235), 1.0f * scale);
  m_guiContext->renderText(label, TextPositioning(center, HorizontalAnchor::HMidAnchor, VerticalAnchor::VMidAnchor));
  m_guiContext->clearTextStyle();
}

void MainInterface::renderWindows() {
  m_paneManager.render();
}

void MainInterface::renderDebug() {
  if (!isDebugDisplayed()) {
    SpatialLogger::clear();
    m_debugTextRect = RectF::null();
    LogMap::clear();
    SpatialLogger::setObserved(false);
    return;
  }
  SpatialLogger::setObserved(true);

  if (m_clientCommandProcessor->debugHudEnabled()) {
    auto assets = Root::singleton().assets();
    m_guiContext->setTextStyle(m_config->debugTextStyle);
    m_guiContext->setLineSpacing(0.5f);

    bool clearMap = m_debugMapClearTimer.wrapTick();
    auto logMapValues = LogMap::getValues();
    if (clearMap)
      LogMap::clear();

    List<String> formatted;
    formatted.reserve(logMapValues.size());

    int counter = 0;
    for (auto const& pair : logMapValues) {
      TextPositioning positioning = { Vec2F(m_config->debugOffset[0], windowHeight() - m_config->debugOffset[1] - m_config->textStyle.fontSize * interfaceScale() * counter++) };
      String& text = formatted.emplace_back(strf("{}^lightgray;:^green,set; {}", pair.first, pair.second));
      m_debugTextRect.combine(m_guiContext->determineTextSize(text, positioning).padded(m_config->debugBackgroundPad));
    }

    if (!m_debugTextRect.isNull()) {
      RenderQuad& quad = m_guiContext->renderer()->immediatePrimitives()
        .emplace_back(std::in_place_type_t<RenderQuad>(), m_debugTextRect, m_config->debugBackgroundColor.toRgba(), 0.0f).get<RenderQuad>();

      quad.b.color[3] = quad.c.color[3] = 0;
    };

    m_debugTextRect = RectF::null();

    for (size_t index = 0; index != formatted.size(); ++index) {
      TextPositioning positioning = { Vec2F(m_config->debugOffset[0], windowHeight() - m_config->debugOffset[1] - m_config->textStyle.fontSize * interfaceScale() * index) };
      m_guiContext->renderText(formatted[index], positioning);
    }
  }

  auto const& camera = m_worldPainter->camera();

  bool clearSpatial = m_debugSpatialClearTimer.wrapTick();

  for (auto const& line : SpatialLogger::getLines("world", clearSpatial)) {
    Vec2F begin = camera.worldToScreen(line.begin);
    Vec2F end = camera.worldGeometry().diff(line.end, line.begin) * camera.pixelRatio() * TilePixels + begin;
    m_guiContext->drawLine(begin, end, line.color, 1);
  }

  for (auto const& line : SpatialLogger::getLines("screen", clearSpatial))
    m_guiContext->drawLine(Vec2F(line.begin), Vec2F(line.end), line.color, 1);

  for (auto const& point : SpatialLogger::getPoints("world", clearSpatial)) {
    auto position = camera.worldToScreen(point.position);
    m_guiContext->drawLine(position + Vec2F(-2, -2), position + Vec2F(-2, 2), point.color, 1);
    m_guiContext->drawLine(position + Vec2F(-2, 2), position + Vec2F(2, 2), point.color, 1);
    m_guiContext->drawLine(position + Vec2F(2, 2), position + Vec2F(2, -2), point.color, 1);
    m_guiContext->drawLine(position + Vec2F(2, -2), position + Vec2F(-2, -2), point.color, 1);
  }

  for (auto const& point : SpatialLogger::getPoints("screen", clearSpatial)) {
    auto position = point.position;
    m_guiContext->drawLine(position + Vec2F(-2, -2), position + Vec2F(-2, 2), point.color, 1);
    m_guiContext->drawLine(position + Vec2F(-2, 2), position + Vec2F(2, 2), point.color, 1);
    m_guiContext->drawLine(position + Vec2F(2, 2), position + Vec2F(2, -2), point.color, 1);
    m_guiContext->drawLine(position + Vec2F(2, -2), position + Vec2F(-2, -2), point.color, 1);
  }

  m_guiContext->setTextStyle(m_config->debugTextStyle);

  for (auto const& logText : SpatialLogger::getText("world", clearSpatial)) {
    m_guiContext->setFontColor(logText.color);
    m_guiContext->renderText(logText.text.utf8Ptr(), camera.worldToScreen(logText.position));
  }

  for (auto const& logText : SpatialLogger::getText("screen", clearSpatial)) {
    m_guiContext->setFontColor(logText.color);
    m_guiContext->renderText(logText.text.utf8Ptr(), logText.position);
  }
  m_guiContext->clearTextStyle();
}

void MainInterface::updateCursor() {
  Maybe<String> cursorOverride = m_actionBar->cursorOverride(m_cursorScreenIPos);

  if (!cursorOverride) {
    if (auto pane = m_paneManager.getPaneAt(m_cursorScreenIPos / interfaceScale())) {
      cursorOverride = cursorOverride.orMaybe(pane->cursorOverride(m_cursorScreenIPos / interfaceScale()));
    } else {
      auto player = m_client->mainPlayer();
      if (auto anchorState = m_client->mainPlayer()->loungingIn()) {
        if (auto loungeable = m_client->worldClient()->get<LoungeableEntity>(anchorState->entityId)) {
          if (auto loungeAnchor = loungeable->loungeAnchor(anchorState->positionIndex))
            cursorOverride = cursorOverride.orMaybe(loungeAnchor->cursorOverride);
        }
      }
      if (!cursorOverride) {
        for (auto item : {player->primaryHandItem(), player->altHandItem()}) {
          if (auto activeItem = as<ActiveItem>(item)) {
            if (auto cursor = activeItem->cursor()) {
              cursorOverride = cursor;
              break;
            }
          } else if (auto inspectionTool = as<InspectionTool>(item)) {
            cursorOverride = String("/cursors/inspect.cursor");
            break;
          }
        }
      }
    }
  }

  if (cursorOverride)
    m_cursor.setCursor(cursorOverride.take());
  else
    m_cursor.resetCursor();
}

void MainInterface::renderCursor() {
  // if we're currently playing a cinematic, we should not render the mouse.
  if (m_cinematicOverlay && !m_cinematicOverlay->completed())
    return m_guiContext->applicationController()->setCursorVisible(false);

  if (!m_cursorVisible)
    return m_guiContext->applicationController()->setCursorVisible(false);

  Vec2I cursorPos = m_cursorScreenIPos;
  Vec2I cursorSize = m_cursor.size();
  Vec2I cursorOffset = m_cursor.offset();
  float cursorScale = m_cursor.scale(interfaceScale());
  Drawable cursorDrawable = m_cursor.drawable();

  cursorPos[0] -= cursorOffset[0] * cursorScale;
  cursorPos[1] -= (cursorSize[1] - cursorOffset[1]) * cursorScale;
#ifdef STAR_SYSTEM_FAMILY_MOBILE
  // Mobile setCursorImage is an unconditional no-op stub, but trySetCursor
  // fetches the cursor image from Assets (a mutex contended with the server
  // thread's world-gen/entity loads) every frame before finding that out.
  // Skip straight to drawing the software cursor.
  m_guiContext->drawDrawable(cursorDrawable, Vec2F(cursorPos), cursorScale);
#else
  if (!m_guiContext->trySetCursor(cursorDrawable, cursorOffset, cursorScale))
    m_guiContext->drawDrawable(cursorDrawable, Vec2F(cursorPos), cursorScale);
#endif

  if (m_cursorTooltip) {
    auto assets = Root::singleton().assets();
    auto imgDb = Root::singleton().imageMetadataDatabase();

    auto config = assets->json("/interface.config:cursorTooltip");
    auto backgroundImage = config.getString("background");
    auto rawCursorOffset = jsonToVec2I(config.get("offset"));

    Vec2I tooltipSize = Vec2I(imgDb->imageSize(backgroundImage)) * interfaceScale();
    Vec2I cursorOffset = (Vec2I{0, -m_cursor.size().y()} + rawCursorOffset) * cursorScale;
    Vec2I tooltipOffset = m_cursorScreenIPos + cursorOffset;
    TextStyle textStyle = config.get("textStyle");
    size_t fontSize = config.get("fontSize").toUInt();
    Vec4B fontColor = jsonToColor(config.get("color")).toRgba();

    m_guiContext->drawQuad(backgroundImage, Vec2F(tooltipOffset) + Vec2F(-tooltipSize.x(), 0), interfaceScale());
    m_guiContext->setTextStyle(textStyle);
    m_guiContext->setFontSize(fontSize);
    m_guiContext->setFontColor(fontColor);
    m_guiContext->renderText(*m_cursorTooltip,
        TextPositioning(Vec2F(tooltipOffset) + Vec2F(-tooltipSize.x(), tooltipSize.y()) / 2,
            HorizontalAnchor::HMidAnchor,
            VerticalAnchor::VMidAnchor));
    m_guiContext->clearTextStyle();
  }

  m_cursorItem->setPosition(Vec2I::round(m_cursorScreenPos / interfaceScale() + Vec2F(m_config->inventoryItemMouseOffset)));

  // Only render the cursor item slot while something is actually being
  // dragged: an EMPTY slot draws no pixels, but Widget::render still sets a
  // scissor rect for it, and a scissor change flushes every pending UI
  // primitive to the GPU mid-batch -- measured at ~3ms/frame of pure flush
  // overhead on mobile GPU drivers, for a widget drawing nothing.
  // On pure-replay frames the sim tick may be mutating the inventory right
  // now; hudSafeToReplay() already guaranteed no drag was in progress at
  // update time, so skip the live swap-slot read entirely.
  if (m_hudForceReplay)
    return;
  if (auto swapItem = m_client->mainPlayer()->inventory()->swapSlotItem()) {
    m_cursorItem->setItem(swapItem);
    m_cursorItem->render(RectI::withSize({}, {(int)windowWidth(), (int)windowHeight()}));
    m_guiContext->resetInterfaceScissorRect();
  } else {
    m_cursorItem->setItem({});
  }
}

bool MainInterface::overButton(PolyI const& buttonPoly, Vec2F const& mousePos) const {
  Vec2F barPos = mainBarPosition();
  PolyF poly(buttonPoly);
  poly.translate(barPos);
  poly.scale(interfaceScale(), Vec2F(barPos));
  return poly.contains(mousePos);
}

bool MainInterface::overlayClick(Vec2F const& mousePos, MouseButton) {
  PolyF mainBarPoly = (PolyF)m_config->mainBarPoly;
  Vec2F barPos = mainBarPosition();
  mainBarPoly.translate(barPos);
  mainBarPoly.scale(interfaceScale(), Vec2F(barPos));

  if (overButton(m_config->mainBarInventoryButtonPoly, mousePos)) {
    m_paneManager.toggleRegisteredPane(MainInterfacePanes::Inventory);
    return true;
  }

  if (overButton(m_config->mainBarCraftButtonPoly, mousePos)) {
    togglePlainCraftingWindow();
    return true;
  }

  if (overButton(m_config->mainBarCodexButtonPoly, mousePos)) {
    m_paneManager.toggleRegisteredPane(MainInterfacePanes::Codex);
    return true;
  }

  if (overButton(m_config->mainBarDeployButtonPoly, mousePos)) {
    if (m_client->canBeamDown(true))
      warpToOrbitedWorld(true);
    else if (m_client->canBeamUp())
      warpToOwnShip();
    return true;
  }

  if (overButton(m_config->mainBarBeamButtonPoly, mousePos)) {
    if (m_client->canBeamDown())
      warpToOrbitedWorld();
    return true;
  }

  if (overButton(m_config->mainBarQuestLogButtonPoly, mousePos)) {
    m_paneManager.toggleRegisteredPane(MainInterfacePanes::QuestLog);
    return true;
  }

  if (overButton(m_config->mainBarMmUpgradeButtonPoly, mousePos)) {
    if (m_client->mainPlayer()->inventory()->essentialItem(EssentialItem::BeamAxe))
      m_paneManager.toggleRegisteredPane(MainInterfacePanes::MmUpgrade);
    return true;
  }

  if (overButton(m_config->mainBarCollectionsButtonPoly, mousePos)) {
    m_paneManager.toggleRegisteredPane(MainInterfacePanes::Collections);
    return true;
  }

  return false;
}

bool MainInterface::handleActionWheelEvent(ActionWheelEvent const& event) {
  if (event.open) {
    m_actionWheelActive = true;
    m_actionWheelType = event.type;
    m_actionWheelSelection = {};
  }

  if (!m_actionWheelActive)
    return event.open || event.confirm || event.cancel;

  updateActionWheelSelection(event.direction);

  if (event.confirm) {
    activateActionWheelSelection();
    m_actionWheelActive = false;
    m_actionWheelType = ActionWheelType::Actions;
    m_actionWheelSelection = {};
  } else if (event.cancel) {
    m_actionWheelActive = false;
    m_actionWheelType = ActionWheelType::Actions;
    m_actionWheelSelection = {};
  }

  return true;
}

bool MainInterface::handleDirectionalAimEvent(DirectionalAimEvent const& event) {
  auto player = m_client->mainPlayer();
  Vec2F anchor = m_worldPainter->camera().worldToScreen(player->position());
  Vec2F target = anchor + event.direction * event.radius;
  target[0] = std::clamp(target[0], 0.0f, (float)windowWidth());
  target[1] = std::clamp(target[1], 0.0f, (float)windowHeight());

  m_cursorScreenIPos = Vec2I::round(m_cursorScreenPos = target);
  m_cursorVisible = event.cursorVisible;
  m_mobileDirectionalAimActive = true;
  return true;
}

List<MainInterface::ActionWheelItem> MainInterface::actionWheelItems() const {
  List<ActionWheelItem> items;

  items.append({ActionWheelAction::Inventory, {}, "Inventory", m_inventoryWindow->containsNewItems() ? m_config->inventoryImageGlow : m_config->inventoryImage, {}, {}, true});
  items.append({ActionWheelAction::Crafting, {}, "Crafting", m_config->craftImage, {}, {}, true});
  items.append({ActionWheelAction::Codex, {}, "Codex", m_config->codexImage, {}, {}, true});
  items.append({ActionWheelAction::QuestLog, {}, "Quests", m_config->questLogImage, {}, {}, true});
  items.append({ActionWheelAction::Collections, {}, "Collections", m_config->collectionsImage, {}, {}, true});

  bool hasMatterManipulator = (bool)m_client->mainPlayer()->inventory()->essentialItem(EssentialItem::BeamAxe);
  items.append({ActionWheelAction::MatterManipulator, {}, "Matter Manipulator", hasMatterManipulator ? m_config->mmUpgradeImage : m_config->mmUpgradeImageDisabled, {}, {}, hasMatterManipulator});

  String swapHotbarImage = m_client->mainPlayer()->inventory()->customBarGroup() != 0
      ? "/interface/actionbar/swapcustombartwo.png"
      : "/interface/actionbar/swapcustombarone.png";
  items.append({ActionWheelAction::SwapHotbar, {}, "Swap Hotbar", swapHotbarImage, {}, {}, true, 1.35f});

  if (m_client->canBeamUp())
    items.append({ActionWheelAction::Deploy, {}, "Beam Up", m_config->beamUpImage, {}, {}, true});
  else
    items.append({ActionWheelAction::Deploy, {}, "Deploy", m_client->canBeamDown(true) ? m_config->deployImage : m_config->deployImageDisabled, {}, {}, m_client->canBeamDown(true)});

  if (m_client->canBeamDown())
    items.append({ActionWheelAction::BeamDown, {}, "Beam Down", m_config->beamDownImage, {}, {}, true});

  return items;
}

List<MainInterface::ActionWheelItem> MainInterface::inventoryWheelItems() const {
  List<ActionWheelItem> items;
  auto inventory = m_client->mainPlayer()->inventory();

  auto addCustomBarItem = [&](CustomBarIndex index) {
    ItemPtr primaryItem;
    ItemPtr secondaryItem;
    if (auto slot = inventory->customBarPrimarySlot(index))
      primaryItem = inventory->itemsAt(*slot);
    if (auto slot = inventory->customBarSecondarySlot(index))
      secondaryItem = inventory->itemsAt(*slot);

    String label = primaryItem ? primaryItem->friendlyName()
        : secondaryItem ? secondaryItem->friendlyName()
        : strf("Slot {}", index + 1);
    items.append({ActionWheelAction::Inventory, SelectedActionBarLocation(index), label, {}, primaryItem, secondaryItem, (bool)(primaryItem || secondaryItem)});
  };

  auto customBarIndexes = inventory->customBarIndexes();
  for (uint8_t i = 0; i < customBarIndexes / 2; ++i)
    addCustomBarItem((CustomBarIndex)i);

  for (uint8_t i = 0; i < EssentialItemCount; ++i) {
    auto item = inventory->essentialItem((EssentialItem)i);
    String label = item ? item->friendlyName() : strf("Tool {}", i + 1);
    items.append({ActionWheelAction::Inventory, SelectedActionBarLocation((EssentialItem)i), label, {}, item, {}, (bool)item});
  }

  for (uint8_t i = customBarIndexes / 2; i < customBarIndexes; ++i)
    addCustomBarItem((CustomBarIndex)i);

  return items;
}

List<MainInterface::ActionWheelItem> MainInterface::activeActionWheelItems() const {
  return m_actionWheelType == ActionWheelType::Inventory ? inventoryWheelItems() : actionWheelItems();
}

void MainInterface::updateActionWheelSelection(Vec2F const& direction) {
  m_actionWheelDirection = direction;
  if (direction.magnitudeSquared() < 0.25f) {
    m_actionWheelSelection = {};
    return;
  }

  auto items = activeActionWheelItems();
  if (items.empty())
    return;

  float angle = std::atan2(direction[1], direction[0]);
  float step = Constants::pi * 2.0f / items.size();
  float wheelAngle = Constants::pi / 2.0f - angle;
  while (wheelAngle < 0.0f)
    wheelAngle += Constants::pi * 2.0f;
  while (wheelAngle >= Constants::pi * 2.0f)
    wheelAngle -= Constants::pi * 2.0f;

  m_actionWheelSelection = (size_t)std::floor((wheelAngle + step / 2.0f) / step) % items.size();
}

void MainInterface::activateActionWheelSelection() {
  auto items = activeActionWheelItems();
  if (!m_actionWheelSelection || *m_actionWheelSelection >= items.size())
    return;

  auto item = items[*m_actionWheelSelection];
  if (!item.enabled)
    return;

  if (item.inventoryLocation)
    activateInventoryWheelLocation(*item.inventoryLocation);
  else
    activateActionWheelAction(item.action);
}

void MainInterface::activateActionWheelAction(ActionWheelAction action) {
  switch (action) {
    case ActionWheelAction::Inventory:
      m_paneManager.toggleRegisteredPane(MainInterfacePanes::Inventory);
      break;
    case ActionWheelAction::Crafting:
      togglePlainCraftingWindow();
      break;
    case ActionWheelAction::Codex:
      m_paneManager.toggleRegisteredPane(MainInterfacePanes::Codex);
      break;
    case ActionWheelAction::QuestLog:
      m_paneManager.toggleRegisteredPane(MainInterfacePanes::QuestLog);
      break;
    case ActionWheelAction::Collections:
      m_paneManager.toggleRegisteredPane(MainInterfacePanes::Collections);
      break;
    case ActionWheelAction::MatterManipulator:
      if (m_client->mainPlayer()->inventory()->essentialItem(EssentialItem::BeamAxe))
        m_paneManager.toggleRegisteredPane(MainInterfacePanes::MmUpgrade);
      break;
    case ActionWheelAction::SwapHotbar:
      m_actionBar->swapHotbar();
      break;
    case ActionWheelAction::Deploy:
      if (m_client->canBeamDown(true))
        warpToOrbitedWorld(true);
      else if (m_client->canBeamUp())
        warpToOwnShip();
      break;
    case ActionWheelAction::BeamDown:
      if (m_client->canBeamDown())
        warpToOrbitedWorld();
      break;
  }
}

void MainInterface::activateInventoryWheelLocation(SelectedActionBarLocation const& location) {
  m_client->mainPlayer()->inventory()->selectActionBarLocation(location);
}

void MainInterface::initHttpTrustDialog() {
  const auto httpTrustDialog = make_shared<HttpTrustDialog>();
  m_paneManager.registerPane(MainInterfacePanes::HttpTrustDialog, PaneLayer::ModalWindow, httpTrustDialog);
}

void MainInterface::displayScriptPane(ScriptPanePtr& scriptPane, EntityId sourceEntity) {
 // keep any number of script panes open with null source entities
  if (sourceEntity != NullEntityId)
    m_interactionScriptPanes[sourceEntity] = scriptPane;

  PaneLayer layer = PaneLayer::Window;
  if (auto layerName = scriptPane->config().optString("paneLayer"))
    layer = PaneLayerNames.getLeft(*layerName);

  if (scriptPane->openWithInventory()) {
    bool closeWithInventory = scriptPane->closeWithInventory();
    m_paneManager.displayPane(layer, scriptPane, [this, closeWithInventory](PanePtr const&) {
      if (closeWithInventory)
        m_paneManager.dismissRegisteredPane(MainInterfacePanes::Inventory);
    });
    m_paneManager.displayRegisteredPane(MainInterfacePanes::Inventory);
    m_paneManager.bringPaneAdjacent(m_paneManager.registeredPane(MainInterfacePanes::Inventory),
        scriptPane, Root::singleton().assets()->json("/interface.config:bringAdjacentWindowGap").toFloat());
  } else {
    m_paneManager.displayPane(layer, scriptPane);
  }
}

}
