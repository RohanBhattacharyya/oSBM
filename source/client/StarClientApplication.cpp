#include "StarClientApplication.hpp"
#include "StarConfiguration.hpp"
#include "StarJsonExtra.hpp"
#include "StarFile.hpp"
#include "StarEncode.hpp"
#include "StarLogging.hpp"
#include "StarJsonExtra.hpp"
#include "StarRoot.hpp"
#include "StarVersion.hpp"
#include "StarPlayer.hpp"
#include "StarPlayerStorage.hpp"
#include "StarPlayerLog.hpp"
#include "StarAssets.hpp"
#include "StarWorldTemplate.hpp"
#include "StarWorldClient.hpp"
#include "StarRootLoader.hpp"
#include "StarInput.hpp"
#include "StarVoice.hpp"
#include "StarCurve25519.hpp"
#include "StarInterpolation.hpp"
#include "StarThread.hpp"

#include "StarCameraLuaBindings.hpp"
#include "StarCelestialLuaBindings.hpp"
#include "StarClipboardLuaBindings.hpp"
#include "StarInputLuaBindings.hpp"
#include "StarInterfaceLuaBindings.hpp"
#include "StarLuaHttpBindings.hpp"
#include "StarRenderingLuaBindings.hpp"
#include "StarTeamClientLuaBindings.hpp"
#include "StarVoiceLuaBindings.hpp"
#include "StarHttpTrustDialog.hpp"
#include "StarMainInterfaceTypes.hpp"
#ifdef STAR_SYSTEM_SWITCH
#include "StarTextBoxWidget.hpp"
#include "mobile/switch/StarSwitchPlatform.hpp"
#endif

#include "imgui.h"
#include "imgui_freetype.h"

#include <algorithm>
#include <cmath>

#ifdef STAR_SYSTEM_SWITCH
#include <malloc.h>
extern "C" void starAllocTrackReport(char* buf, size_t bufSize);
#endif

#if defined STAR_SYSTEM_WINDOWS
#include <windows.h>
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = 1;
extern "C" __declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
#endif // graphics driver is told by these exports to default to the dedicated GPU

namespace Star {

void setMobileStartupStatus(String const& status);

Json const AdditionalAssetsSettings = Json::parseJson(R"JSON(
    {
      "missingImage" : "/assetmissing.png",
      "missingAudio" : "/assetmissing.wav"
    }
  )JSON");

namespace {

bool mobileDefaultActionMatch(Key key, InterfaceAction action) {
  switch (action) {
    case InterfaceAction::PlayerUp:
      return key == Key::W;
    case InterfaceAction::PlayerDown:
      return key == Key::S;
    case InterfaceAction::PlayerLeft:
      return key == Key::A;
    case InterfaceAction::PlayerRight:
      return key == Key::D;
    case InterfaceAction::PlayerJump:
      return key == Key::Space;
    case InterfaceAction::PlayerInteract:
      return key == Key::E;
    case InterfaceAction::InterfaceEscapeMenu:
      return key == Key::Escape;
    default:
      return false;
  }
}

}

Json const AdditionalDefaultConfiguration = Json::parseJson(R"JSON(
    {
      "configurationVersion" : {
        "client" : 8
      },

      "allowAssetsMismatch" : false,
      "vsync" : true,
      "fpsCap" : 0,
      "renderInterpolation" : true,
      "limitTextureAtlasSize" : false,
      "useMultiTexturing" : true,
      "audioChannelSeparation" : [-25, 25],

      "sfxVol" : 100,
      "instrumentVol" : 100,
      "musicVol" : 70,
      "hardwareCursor" : true,
      "windowedResolution" : [1000, 600],
      "fullscreenResolution" : [1920, 1080],
      "fullscreen" : false,
      "borderless" : false,
      "maximized" : true,
      "antiAliasing" : false,
      "zoomLevel" : 3.0,
      "cameraSpeedFactor" : 1.0,
      "interfaceScale" : 0,
      "speechBubbles" : true,
      "mobile" : {
        "touchControls" : {
          "enabled" : true,
          "opacity" : 0.35,
          "size" : 1.0,
          "deadzone" : 0.15,
          "invertLook" : false
        }
      },

      "title" : {
        "multiPlayerAddress" : "",
        "multiPlayerPort" : "",
        "multiPlayerAccount" : "",
        "multiPlayerForceLegacy" : false
      },

      "bindings" : {
        "PlayerUp" :  [ { "type" : "key", "value" : "W", "mods" : [] } ],
        "PlayerDown" :  [ { "type" : "key", "value" : "S", "mods" : [] } ],
        "PlayerLeft" :  [ { "type" : "key", "value" : "A", "mods" : [] } ],
        "PlayerRight" :  [ { "type" : "key", "value" : "D", "mods" : [] } ],
        "PlayerJump" :  [ { "type" : "key", "value" : "Space", "mods" : [] } ],
        "PlayerDropItem" :  [ { "type" : "key", "value" : "Q", "mods" : [] } ],
        "PlayerInteract" :  [ { "type" : "key", "value" : "E", "mods" : [] } ],
        "PlayerShifting" :  [ { "type" : "key", "value" : "RShift", "mods" : [] }, { "type" : "key", "value" : "LShift", "mods" : [] } ],
        "PlayerTechAction1" :  [ { "type" : "key", "value" : "F", "mods" : [] } ],
        "PlayerTechAction2" :  [],
        "PlayerTechAction3" :  [],
        "EmoteBlabbering" :  [ { "type" : "key", "value" : "Right", "mods" : ["LCtrl", "LShift"] } ],
        "EmoteShouting" :  [ { "type" : "key", "value" : "Up", "mods" : ["LCtrl", "LAlt"] } ],
        "EmoteHappy" :  [ { "type" : "key", "value" : "Up", "mods" : [] } ],
        "EmoteSad" :  [ { "type" : "key", "value" : "Down", "mods" : [] } ],
        "EmoteNeutral" :  [ { "type" : "key", "value" : "Left", "mods" : [] } ],
        "EmoteLaugh" :  [ { "type" : "key", "value" : "Left", "mods" : [ "LCtrl" ] } ],
        "EmoteAnnoyed" :  [ { "type" : "key", "value" : "Right", "mods" : [] } ],
        "EmoteOh" :  [ { "type" : "key", "value" : "Right", "mods" : [ "LCtrl" ] } ],
        "EmoteOooh" :  [ { "type" : "key", "value" : "Down", "mods" : [ "LCtrl" ] } ],
        "EmoteBlink" :  [ { "type" : "key", "value" : "Up", "mods" : [ "LCtrl" ] } ],
        "EmoteWink" :  [ { "type" : "key", "value" : "Up", "mods" : ["LCtrl", "LShift"] } ],
        "EmoteEat" :  [ { "type" : "key", "value" : "Down", "mods" : ["LCtrl", "LShift"] } ],
        "EmoteSleep" :  [ { "type" : "key", "value" : "Left", "mods" : ["LCtrl", "LShift"] } ],
        "ShowLabels" :  [ { "type" : "key", "value" : "RAlt", "mods" : [] }, { "type" : "key", "value" : "LAlt", "mods" : [] } ],
        "CameraShift" :  [ { "type" : "key", "value" : "RCtrl", "mods" : [] }, { "type" : "key", "value" : "LCtrl", "mods" : [] } ],
        "TitleBack" :  [ { "type" : "key", "value" : "Esc", "mods" : [] } ],
        "CinematicSkip" :  [ { "type" : "key", "value" : "Esc", "mods" : [] } ],
        "CinematicNext" :  [ { "type" : "key", "value" : "Right", "mods" : [] }, { "type" : "key", "value" : "Return", "mods" : [] } ],
        "GuiClose" :  [ { "type" : "key", "value" : "Esc", "mods" : [] } ],
        "GuiShifting" :  [ { "type" : "key", "value" : "RShift", "mods" : [] }, { "type" : "key", "value" : "LShift", "mods" : [] } ],
        "KeybindingCancel" :  [ { "type" : "key", "value" : "Esc", "mods" : [] } ],
        "KeybindingClear" :  [ { "type" : "key", "value" : "Del", "mods" : [] }, { "type" : "key", "value" : "Backspace", "mods" : [] } ],
        "ChatPageUp" :  [ { "type" : "key", "value" : "PageUp", "mods" : [] } ],
        "ChatPageDown" :  [ { "type" : "key", "value" : "PageDown", "mods" : [] } ],
        "ChatPreviousLine" :  [ { "type" : "key", "value" : "Up", "mods" : [] } ],
        "ChatNextLine" :  [ { "type" : "key", "value" : "Down", "mods" : [] } ],
        "ChatSendLine" :  [ { "type" : "key", "value" : "Return", "mods" : [] } ],
        "ChatBegin" :  [ { "type" : "key", "value" : "Return", "mods" : [] } ],
        "ChatBeginCommand" :  [ { "type" : "key", "value" : "/", "mods" : [] } ],
        "ChatStop" :  [ { "type" : "key", "value" : "Esc", "mods" : [] } ],
        "InterfaceHideHud" :  [ { "type" : "key", "value" : "F1", "mods" : [] } ],
        "InterfaceChangeBarGroup" :  [ { "type" : "key", "value" : "X", "mods" : [] } ],
        "InterfaceDeselectHands" :  [ { "type" : "key", "value" : "Z", "mods" : [] } ],
        "InterfaceBar1" :  [ { "type" : "key", "value" : "1", "mods" : [] } ],
        "InterfaceBar2" :  [ { "type" : "key", "value" : "2", "mods" : [] } ],
        "InterfaceBar3" :  [ { "type" : "key", "value" : "3", "mods" : [] } ],
        "InterfaceBar4" :  [ { "type" : "key", "value" : "4", "mods" : [] } ],
        "InterfaceBar5" :  [ { "type" : "key", "value" : "5", "mods" : [] } ],
        "InterfaceBar6" :  [ { "type" : "key", "value" : "6", "mods" : [] } ],
        "InterfaceBar7" :  [],
        "InterfaceBar8" :  [],
        "InterfaceBar9" :  [],
        "InterfaceBar10" :  [],
        "EssentialBar1" :  [ { "type" : "key", "value" : "R", "mods" : [] } ],
        "EssentialBar2" :  [ { "type" : "key", "value" : "T", "mods" : [] } ],
        "EssentialBar3" :  [ { "type" : "key", "value" : "Y", "mods" : [] } ],
        "EssentialBar4" :  [ { "type" : "key", "value" : "N", "mods" : [] } ],
        "InterfaceRepeatCommand" :  [ { "type" : "key", "value" : "P", "mods" : [] } ],
        "InterfaceToggleFullscreen" :  [ { "type" : "key", "value" : "F11", "mods" : [] } ],
        "InterfaceReload" :  [],
        "InterfaceEscapeMenu" :  [ { "type" : "key", "value" : "Esc", "mods" : [] } ],
        "InterfaceInventory" :  [ { "type" : "key", "value" : "I", "mods" : [] } ],
        "InterfaceCodex" :  [ { "type" : "key", "value" : "L", "mods" : [] } ],
        "InterfaceQuest" :  [ { "type" : "key", "value" : "J", "mods" : [] } ],
        "InterfaceCrafting" :  [ { "type" : "key", "value" : "C", "mods" : [] } ]
      }
    }
  )JSON");

void ClientApplication::startup(StringList const& cmdLineArgs) {
  setMobileStartupStatus("Parsing mobile boot configuration...");
  RootLoader rootLoader({AdditionalAssetsSettings, AdditionalDefaultConfiguration, String("starbound.log"), LogLevel::Info, false, String("starbound.config")});
  setMobileStartupStatus("Initializing engine root (log rotation)...");
  m_root = rootLoader.initOrDie(cmdLineArgs).first;
  setMobileStartupStatus("Engine root ready...");

#if STAR_SYSTEM_ANDROID || STAR_SYSTEM_IOS
  setMobileStartupStatus("Opening asset index...");
  auto assets = m_root->assets();
  setMobileStartupStatus("Loading interface scale configuration...");
  m_minInterfaceScale = assets->json("/interface.config:minInterfaceScale").toFloat();
  m_maxInterfaceScale = assets->json("/interface.config:maxInterfaceScale").toFloat();
  m_crossoverRes = jsonToVec2F(assets->json("/interface.config:interfaceCrossoverRes"));
  setMobileStartupStatus("Loading client configuration...");
  m_windowTitle = assets->json("/client.config:windowTitle").toString();
  m_maxFrameSkipSetting = assets->json("/client.config:maxFrameSkip").toUInt();
  m_updateTrackWindowSetting = assets->json("/client.config:updateTrackWindow").toFloat();
  m_assetsBootstrapReady = true;
  setMobileStartupStatus("Finished loading assets and configuration...");
#endif

  Logger::info("OpenStarbound Client v{} for v{} ({}) Source ID: {} Protocol: {}", OpenStarVersionString, StarVersionString, StarArchitectureString, StarSourceIdentifierString, StarProtocolVersion);
}

#ifdef STAR_SYSTEM_SWITCH
void ClientApplication::startSimTick() {
  if (!m_simTickPending)
    return;
  m_simTickPending = false;
  MutexLocker locker(m_simMutex);
  if (!m_simThread) {
    m_simThread = Thread::invoke("ClientApplication::simTick", [this]() {
        MutexLocker locker(m_simMutex);
        while (true) {
          while (!m_simRunRequested && !m_simShutdown)
            m_simCond.wait(m_simMutex);
          if (m_simShutdown)
            return;
          m_simRunRequested = false;
          locker.unlock();
          int64_t tickStart = Time::monotonicMicroseconds();
          try {
            m_universeClient->setProcessingOnSimWorker(true);
            m_universeClient->update(m_simTickDt);
          } catch (...) {
            m_simException = std::current_exception();
          }
          m_universeClient->setProcessingOnSimWorker(false);
          m_simLastTickUs = Time::monotonicMicroseconds() - tickStart;
          locker.lock();
          m_simRunning = false;
          m_simCond.broadcast();
        }
      });
  }
  m_simRunning = true;
  m_simRunRequested = true;
  m_simTickInFlight = true;
  m_simCond.broadcast();
}

void ClientApplication::joinSimTick() {
  // A tick that was deferred but never started (world path skipped this
  // frame) still has to run, or the sim would silently stall.
  if (m_simTickPending) {
    m_simTickPending = false;
    m_universeClient->update(m_simTickDt);
    return;
  }
  m_simTickInFlight = false;
  MutexLocker locker(m_simMutex);
  while (m_simRunning)
    m_simCond.wait(m_simMutex);
  if (m_simException) {
    auto e = m_simException;
    m_simException = nullptr;
    std::rethrow_exception(e);
  }
}

void ClientApplication::stopSimThread() {
  {
    MutexLocker locker(m_simMutex);
    if (!m_simThread)
      return;
    while (m_simRunning)
      m_simCond.wait(m_simMutex);
    m_simShutdown = true;
    m_simCond.broadcast();
  }
  m_simThread.finish();
  m_simThread = {};
  m_simShutdown = false;
}
#endif

void ClientApplication::shutdown() {
  // Clear HTTP trust request callback
  LuaBindings::clearHttpTrustRequestCallback();

#ifdef STAR_SYSTEM_SWITCH
  stopSimThread();
#endif

  m_mainInterface.reset();
  m_player.reset();

  if (m_universeClient)
    m_universeClient->disconnect();

  if (m_universeServer) {
    m_universeServer->stop();
    m_universeServer->join();
    m_universeServer.reset();
  }

  if (m_statistics) {
    m_statistics->writeStatistics();
    m_statistics.reset();
  }

  m_universeClient.reset();
  m_statistics.reset();
  m_playerStorage.reset();
  m_titleScreen.reset();
  m_worldPainter.reset();
  m_cinematicOverlay.reset();
  m_errorScreen.reset();

  // Finish any async root loader thread before tearing down Root.
  if (m_rootLoader)
    m_rootLoader.finish();
  m_reloadListener.reset();

  // Destroy singletons before Root since their destructors may access Root.
  m_voice.reset();
  m_input.reset();
  m_guiContext.reset();
  m_mainMixer.reset();

  // Reset app state so a subsequent startup()/applicationInit()/renderInit()
  // cycle works correctly (supports relaunch without process restart).
  m_state = MainAppState::Startup;

  // Destroy Root last; this clears RootBase::s_singleton so the next
  // startup() call can construct a fresh Root.
  m_root.reset();
}

void ClientApplication::applicationInit(ApplicationControllerPtr appController) {
  Application::applicationInit(appController);
  appController->setCursorVisible(true);
  auto configuration = m_root->configuration();
  bool vsync = configuration->get("vsync").toBool();
  Vec2U windowedSize = jsonToVec2U(configuration->get("windowedResolution"));
  Vec2U fullscreenSize = jsonToVec2U(configuration->get("fullscreenResolution"));
  bool fullscreen = configuration->get("fullscreen").toBool();
  bool borderless = configuration->get("borderless").toBool();
  bool maximized = configuration->get("maximized").toBool();
  m_controllerInput = configuration->get("controllerInput").optBool().value();
  if (fullscreen)
    appController->setFullscreenWindow(fullscreenSize);
  else if (borderless)
    appController->setBorderlessWindow();
  else if (maximized)
    appController->setMaximizedWindow();
  else
    appController->setNormalWindow(windowedSize);

  float updateRate = 1.0f / GlobalTimestep;
  if (auto jUpdateRate = configuration->get("updateRate")) {
    updateRate = jUpdateRate.toFloat();
    GlobalTimestep = 1.0f / updateRate;
  }

  if (auto jServerUpdateRate = configuration->get("serverUpdateRate"))
    ServerGlobalTimestep = 1.0f / jServerUpdateRate.toFloat();
  appController->setTargetUpdateRate(updateRate);
  // Rendered-FPS cap, independent of the sim rate; 0 (the default) is
  // unlimited -- powerful machines render as fast as vsync/platform allows,
  // while a cap keeps weaker machines and batteries comfortable.
  appController->setMaxFrameRate(configuration->get("fpsCap").optFloat().value(0.0f));
  // Sub-tick render interpolation keeps motion crisp when rendering faster
  // than the sim rate (costs one sim tick of render latency).
  m_renderInterpolation = configuration->get("renderInterpolation").optBool().value(true);
  Logger::info("Client build compiled {} {}", __DATE__, __TIME__);
  appController->setVSyncEnabled(vsync);
  appController->setCursorHardware(configuration->get("hardwareCursor").optBool().value(true));

  // Must be called before anything that can invoke an asset load.
  loadMods();
  AudioFormat audioFormat = appController->enableAudio();
  m_mainMixer = make_shared<MainMixer>(audioFormat.sampleRate, audioFormat.channels);
  m_mainMixer->setVolume(0.5);
  m_guiContext = make_shared<GuiContext>(m_mainMixer->mixer(), appController);
  m_input = make_shared<Input>();
#if !STAR_SYSTEM_ANDROID && !STAR_SYSTEM_IOS
  m_voice = make_shared<Voice>(appController);
#else
  m_voice.reset();
#endif
#if STAR_SYSTEM_ANDROID || STAR_SYSTEM_IOS
  if (m_assetsBootstrapReady) {
    appController->setApplicationTitle(m_windowTitle);
    appController->setMaxFrameSkip(m_maxFrameSkipSetting);
    appController->setUpdateTrackWindow(m_updateTrackWindowSetting);
  }
#else
  auto assets = m_root->assets();
  {
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    m_immediateFont = *assets->bytes("/hobo.ttf");
    ImFontConfig config{};
    config.FontDataOwnedByAtlas = false;
    config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_ForceAutoHint;
    io.Fonts->AddFontFromMemoryTTF(m_immediateFont.ptr(), m_immediateFont.size(),
      16, &config, io.Fonts->GetGlyphRangesDefault());
  }
  m_minInterfaceScale = assets->json("/interface.config:minInterfaceScale").toFloat();
  m_maxInterfaceScale = assets->json("/interface.config:maxInterfaceScale").toFloat();
  m_crossoverRes = jsonToVec2F(assets->json("/interface.config:interfaceCrossoverRes"));
  appController->setApplicationTitle(assets->json("/client.config:windowTitle").toString());
  appController->setMaxFrameSkip(assets->json("/client.config:maxFrameSkip").toUInt());
  appController->setUpdateTrackWindow(assets->json("/client.config:updateTrackWindow").toFloat());
#endif
  if (m_voice) {
    if (auto jVoice = configuration->get("voice"))
      m_voice->loadJson(jVoice.toObject(), true);

    m_voice->init();
    m_voice->setLocalSpeaker(0);
  }
}

void ClientApplication::renderInit(RendererPtr renderer) {
  setMobileStartupStatus("Renderer: binding application renderer...");
  Application::renderInit(renderer);
  if (!m_worldPainter) {
    setMobileStartupStatus("Renderer: preparing world painter...");
    m_worldPainter = make_shared<WorldPainter>();
  }
  setMobileStartupStatus("Renderer: loading effects and framebuffers...");
  renderReload();
  m_root->registerReloadListener(m_reloadListener = make_shared<CallbackListener>([this]() { renderReload(); }));

  setMobileStartupStatus("Renderer: applying mobile render settings...");
  if (m_root->configuration()->get("limitTextureAtlasSize").optBool().value(false))
    renderer->setSizeLimitEnabled(true);

  renderer->setMultiTexturingEnabled(m_root->configuration()->get("useMultiTexturing").optBool().value(true));

  setMobileStartupStatus("Renderer: preparing interface textures...");
  m_guiContext->renderInit(renderer);

  setMobileStartupStatus("Renderer: preparing startup overlays...");
  m_cinematicOverlay = make_shared<Cinematic>();
  m_errorScreen = make_shared<ErrorScreen>();

  if (m_titleScreen)
    m_titleScreen->renderInit(renderer);
#if STAR_SYSTEM_IOS
  // The world renderer is not used until gameplay. Deferring this on iOS keeps
  // the launch path out of several large GLES texture-group allocations while
  // preserving the later initialization path used before entering a world.
  setMobileStartupStatus("Renderer: deferred world rendering until gameplay...");
#else
  if (m_worldPainter)
    m_worldPainter->renderInit(renderer);
#endif

  #ifdef STAR_ENABLE_STEAM_INTEGRATION
  #ifdef STAR_SYSTEM_LINUX
  if (g_steamIsFlatpak) {
    auto config = m_root->configuration();
    if (!config->get("steamFlatpakWarningShown").optBool().value()) {
      config->set("steamFlatpakWarningShown", true);
      m_errorScreen->setMessage(m_root->assets()->json("/interface.config:steamFlatpakWarning").toString());
      changeState(MainAppState::SteamFlatpakWarning);
      return;
    }
  }
  #endif
  #endif

  setMobileStartupStatus("Renderer: entering startup flow...");
  changeState(MainAppState::Mods);
}

void ClientApplication::windowChanged(WindowMode windowMode, Vec2U screenSize) {
#if STAR_SYSTEM_ANDROID || STAR_SYSTEM_IOS
  _unused(windowMode);
  _unused(screenSize);
#else
  auto config = m_root->configuration();
  if (windowMode == WindowMode::Fullscreen) {
    config->set("fullscreenResolution", jsonFromVec2U(screenSize));
    config->set("fullscreen", true);
    config->set("borderless", false);
  } else if (windowMode == WindowMode::Borderless) {
    config->set("borderless", true);
    config->set("fullscreen", false);
  } else if (windowMode == WindowMode::Maximized) {
    config->set("maximized", true);
    config->set("fullscreen", false);
    config->set("borderless", false);
  } else {
    config->set("maximized", false);
    config->set("fullscreen", false);
    config->set("borderless", false);
    config->set("windowedResolution", jsonFromVec2U(screenSize));
  }
#endif
}

void ClientApplication::processInput(InputEvent const& event) {
  if (auto keyDown = event.ptr<KeyDownEvent>()) {
    m_heldKeyEvents.append(*keyDown);
    m_edgeKeyEvents.append(*keyDown);
  } else if (auto keyUp = event.ptr<KeyUpEvent>()) {
    eraseWhere(m_heldKeyEvents, [&](auto& keyEvent) {
      return keyEvent.key == keyUp->key;
    });

    Maybe<KeyMod> modKey = KeyModNames.maybeLeft(KeyNames.getRight(keyUp->key));
    if (modKey)
      m_heldKeyEvents.transform([&](auto& keyEvent) {
        return KeyDownEvent{keyEvent.key, keyEvent.mods & ~*modKey};
      });
  }
  else if (auto cAxis = event.ptr<ControllerAxisEvent>()) {
    float axisValue = cAxis->controllerAxisValue;
#if STAR_SYSTEM_ANDROID || STAR_SYSTEM_IOS
    // Mobile platforms can report noisy virtual/driver axis values.
    // Filter tiny magnitudes so they do not unintentionally slow movement.
    if (abs(axisValue) < 0.25f)
      axisValue = 0.0f;
#endif

    if (cAxis->controllerAxis == ControllerAxis::LeftX)
      m_controllerLeftStick[0] = axisValue;
    else if (cAxis->controllerAxis == ControllerAxis::LeftY)
      m_controllerLeftStick[1] = axisValue;
    else if (cAxis->controllerAxis == ControllerAxis::RightX)
      m_controllerRightStick[0] = axisValue;
    else if (cAxis->controllerAxis == ControllerAxis::RightY)
      m_controllerRightStick[1] = axisValue;
  }

  bool processed = !m_errorScreen->accepted() && m_errorScreen->handleInputEvent(event);

  if (!processed) {
    if (m_state == MainAppState::Splash) {
      processed = m_cinematicOverlay->handleInputEvent(event);
    } else if (m_state == MainAppState::Title) {
      if (!(processed = m_cinematicOverlay->handleInputEvent(event)))
        processed = m_titleScreen->handleInputEvent(event);

    } else if (m_state == MainAppState::SinglePlayer || m_state == MainAppState::MultiPlayer) {
#if STAR_SYSTEM_ANDROID || STAR_SYSTEM_IOS
      // On mobile, route gameplay UI input first so touch interaction keeps
      // working even if a cinematic overlay reports suppressInput.  Escape is
      // still a real Escape key, though, so cinematics get first chance to
      // consume it for skipping before the pause dialog sees it.
      if (auto keyDown = event.ptr<KeyDownEvent>(); keyDown && keyDown->key == Key::Escape)
        processed = m_cinematicOverlay->handleInputEvent(event);
      if (!processed && !(processed = m_mainInterface->handleInputEvent(event)))
        processed = m_cinematicOverlay->handleInputEvent(event);
#else
      if (!(processed = m_cinematicOverlay->handleInputEvent(event)))
        processed = m_mainInterface->handleInputEvent(event);
#endif
    }
  }

  if (m_input)
    m_input->handleInput(event, processed);
}

void ClientApplication::update() {
  float dt = GlobalTimestep * GlobalTimescale;
  auto& app = appController();
  if (m_state >= MainAppState::Title) {
    if (auto p2pNetworkingService = app->p2pNetworkingService()) {
      if (auto join = p2pNetworkingService->pullPendingJoin()) {
        m_pendingMultiPlayerConnection = PendingMultiPlayerConnection{join.takeValue(), {}, {}, false};
        changeState(MainAppState::Title);
      }
      
      if (auto req = p2pNetworkingService->pullJoinRequest())
        m_mainInterface->queueJoinRequest(*req);

      p2pNetworkingService->update();
    }
  }

  if (!m_errorScreen->accepted())
    m_errorScreen->update(dt);

  // This warning is only applicable to Linux systems so no need to process it otherwise.
  #ifdef STAR_ENABLE_STEAM_INTEGRATION
  #ifdef STAR_SYSTEM_LINUX
  if (m_state == MainAppState::SteamFlatpakWarning)
    updateSteamFlatpakWarning(dt);
  else
  #endif
  #endif

  if (m_state == MainAppState::Mods)
    updateMods(dt);
  else if (m_state == MainAppState::ModsWarning)
    updateModsWarning(dt);

  if (m_state == MainAppState::Splash)
    updateSplash(dt);
  else if (m_state == MainAppState::Error)
    updateError(dt);
  else if (m_state == MainAppState::Title)
    updateTitle(dt);
  else if (m_state > MainAppState::Title)
    updateRunning(dt);
  
  // Swallow leftover encoded voice data if we aren't in-game to allow mic read to continue for settings.
  if (m_voice && m_state <= MainAppState::Title) {
    DataStreamBuffer ext;
    m_voice->send(ext);
  } // TODO: directly disable encoding at menu so we don't have to do this

  m_guiContext->cleanup();
  m_edgeKeyEvents.clear();
  if (m_input)
    m_input->update();
  ++m_framesSkipped;
}

void ClientApplication::render() {
  m_framesSkipped = 0;
  auto config = m_root->configuration();
  auto assets = m_root->assets();
  auto& renderer = Application::renderer();

  renderer->setMultiSampling(config->get("antiAliasing").optBool().value(false) ? 4 : 0);
  renderer->switchEffectConfig("interface");

  if (auto interfaceScale = config->get("interfaceScale").optFloat().value(); interfaceScale != 0)
    m_guiContext->setInterfaceScale(interfaceScale);
#if STAR_SYSTEM_ANDROID || STAR_SYSTEM_IOS
  else {
    float shortSide = std::min(m_guiContext->windowWidth(), m_guiContext->windowHeight());
    m_guiContext->setInterfaceScale(std::clamp(shortSide / 500.0f, 1.35f, 2.4f));
  }
#else
  else if (m_guiContext->windowWidth() >= m_crossoverRes[0] && m_guiContext->windowHeight() >= m_crossoverRes[1])
    m_guiContext->setInterfaceScale(m_maxInterfaceScale);
  else
    m_guiContext->setInterfaceScale(m_minInterfaceScale);
#endif

  if (m_state == MainAppState::Mods || m_state == MainAppState::Splash) {
    m_cinematicOverlay->render();

  } else if (m_state == MainAppState::Title) {
    m_titleScreen->render();
    m_cinematicOverlay->render();

  } else if (m_state > MainAppState::Title) {
#ifdef STAR_SYSTEM_SWITCH
    // Pipeline profiling: frame interval plus the phase laps that matter for
    // the overlapped sim/paint schedule. One aggregated log line per ~5s.
    static int64_t s_frames = 0, s_lastFrameStart = 0;
    static int64_t s_frameUs = 0, s_snapUs = 0, s_paintUs = 0, s_joinUs = 0, s_ifaceUs = 0, s_simUs = 0;
    int64_t perfNow = Time::monotonicMicroseconds();
    if (s_lastFrameStart)
      s_frameUs += perfNow - s_lastFrameStart;
    s_lastFrameStart = perfNow;
    int64_t perfLap = perfNow;
    auto lap = [&perfLap](int64_t& acc) {
      int64_t n = Time::monotonicMicroseconds();
      acc += n - perfLap;
      perfLap = n;
    };
#endif
    WorldClientPtr worldClient = m_universeClient->worldClient();
    if (worldClient) {
#if !STAR_SYSTEM_ANDROID && !STAR_SYSTEM_IOS
      auto totalStart = Time::monotonicMicroseconds();
#endif
      renderer->switchEffectConfig("world");

      // Sub-tick render interpolation: frames rendered between sim ticks
      // draw the world at lerp(previous tick, current tick, alpha) -- camera
      // here, entities via per-entity offsets computed in the snapshot below,
      // particles via velocity extrapolation in the painter. Without this,
      // rendering faster than the sim rate shows each sim step for several
      // frames (double-image ghosting on moving content).
      float renderAlpha = m_renderInterpolation ? appController()->updateTickFraction() : 1.0f;
      float worldAlpha = renderAlpha;
#ifdef STAR_SYSTEM_SWITCH
      // Deferred-pipeline phase correction: on frames that scheduled a sim
      // tick, the snapshot below is taken BEFORE that tick runs (it executes
      // on the worker during paint), so the snapshot lags the loop's alpha
      // accumulator by one tick; on zero-update frames it does not lag at
      // all (the previous frame's deferred tick completed during its paint).
      // Rendering with the raw alpha therefore oscillates a full tick
      // between the two frame kinds -- measured as the player sprite
      // jittering at walk speed while the (smoothing-filtered) camera and
      // world stayed smooth. Shifting the ENTITY/PARTICLE alpha back one
      // tick on the frames whose snapshot already contains the newest tick
      // makes rendered time uniformly (scheduledTicks - 2 + alpha): one
      // extra tick of constant latency, linear in wall time. Negative alpha
      // extrapolates one keyframe pair back through the same lerp formula
      // (exact at constant velocity).
      //
      // The CAMERA keeps the raw alpha: its keyframe pair is captured in
      // updateCamera (pre-tick, update-frames only), so it freezes on
      // zero-update frames exactly when the entity pair advances -- the two
      // opposite one-tick offsets cancel, and both timelines land on
      // (scheduledTicks - 2 + alpha) with their respective alphas.
      if (m_renderInterpolation && !m_simTickPending)
        worldAlpha -= 1.0f;
#endif
      worldClient->setRenderInterpolationAlpha(worldAlpha);
      m_worldPainter->setRenderInterpolationAlpha(worldAlpha);
      if (renderAlpha < 1.0f && m_cameraHistoryValid) {
        auto const& geometry = worldClient->geometry();
        Vec2F cameraDelta = geometry.diff(m_cameraCurPosition, m_cameraPrevPosition);
        if (cameraDelta != Vec2F() && cameraDelta.magnitude() < 16.0f) // teleports snap
          m_worldPainter->setCameraPosition(geometry, m_cameraCurPosition + cameraDelta * (renderAlpha - 1.0f));
      }

#if !STAR_SYSTEM_ANDROID && !STAR_SYSTEM_IOS
      auto clientStart = totalStart;
#endif
      worldClient->render(m_renderData, TilePainter::BorderTileSize);
#ifdef STAR_SYSTEM_SWITCH
      lap(s_snapUs);
#endif
#ifdef STAR_SYSTEM_SWITCH
      // renderData is now a self-contained snapshot (copied tiles/sky/
      // particles, shared-immutable entity drawables), so the deferred sim
      // tick can run on the worker while the GL-only paint below proceeds.
      if (m_simTickPending)
        startSimTick();
#endif
#if !STAR_SYSTEM_ANDROID && !STAR_SYSTEM_IOS
      LogMap::set("client_render_world_client", strf(u8"{:05d}\u00b5s", Time::monotonicMicroseconds() - clientStart));
#endif

#if !STAR_SYSTEM_ANDROID && !STAR_SYSTEM_IOS
      auto paintStart = Time::monotonicMicroseconds();
#endif
      m_worldPainter->render(m_renderData, [&]() -> bool {
        return worldClient->waitForLighting(&m_renderData);
      });
#if !STAR_SYSTEM_ANDROID && !STAR_SYSTEM_IOS
      LogMap::set("client_render_world_painter", strf(u8"{:05d}\u00b5s", Time::monotonicMicroseconds() - paintStart));
      LogMap::set("client_render_world_total", strf(u8"{:05d}\u00b5s", Time::monotonicMicroseconds() - totalStart));
#endif

#ifdef STAR_SYSTEM_FAMILY_MOBILE
      // With no post-process layers nothing ever samples the "main"
      // framebuffer, so route the world render directly to the screen --
      // saves a fullscreen blit + clear + render-target switch every frame
      // (significant on translated drivers and mobile tile GPUs).  Re-checked
      // every frame so enabling a post-process mod restores the intermediate.
      renderer->setFrameBufferBypass("main", m_postProcessLayers.empty());
#endif
      auto size = Vec2F(renderer->screenSize());
      auto quad = renderFlatRect(RectF::withSize(size / -2, size), Vec4B::filled(0), 0.0f);
      for (auto& layer : m_postProcessLayers) {
        if (layer.group ? layer.group->enabled : true) {
          for (unsigned i = 0; i < layer.passes; i++) {
            for (auto& effect : layer.effects) {
              renderer->switchEffectConfig(effect);
              renderer->render(quad);
            }
          }
        }
      }
#ifdef STAR_SYSTEM_SWITCH
      lap(s_paintUs);
#endif
    }
#ifdef STAR_SYSTEM_SWITCH
    // Sync-frame HUD scheme: fresh HUD renders read live game state, so they
    // require the sim tick to be complete -- but a HUD frame that is a pure
    // REPLAY of recorded primitives (panes, bar group, cached in-world
    // painters) reads none, and can safely run while the sim tick is still
    // executing. Every 3rd frame is a sync frame (join now, render fresh,
    // re-record); in between, if the whole HUD can replay, defer the join to
    // after the UI phase, extending the sim tick's overlap window from just
    // the world paint to nearly the entire frame.
    bool hudReplay = false;
    if (m_simTickInFlight && m_mainInterface) {
      ++m_hudSyncCounter;
      hudReplay = (m_hudSyncCounter % 3 != 0)
          && m_hudFbValid
          && m_cinematicOverlay->completed()
          && m_errorScreen->accepted()
          && m_mainInterface->hudSafeToReplay();
    }
    if (!hudReplay)
      joinSimTick();
    lap(s_joinUs);
    if (m_mainInterface)
      m_mainInterface->setHudForceReplay(hudReplay);
#endif
    renderer->switchEffectConfig("interface");
#if !STAR_SYSTEM_ANDROID && !STAR_SYSTEM_IOS
    auto start = Time::monotonicMicroseconds();
#endif
    m_mainInterface->renderInWorldElements();
#ifdef STAR_SYSTEM_SWITCH
    // HUD framebuffer path: sync frames render the interface fresh into the
    // preserved "hud" overlay buffer and composite it; the frames in between
    // just composite the previous contents (one textured quad) with only the
    // cursor drawn live on top. Falls back to plain rendering if the buffer
    // is unavailable.
    if (hudReplay) {
      renderer->compositeFrameBufferToCurrent("hud");
      m_mainInterface->renderCursorOverlay();
    } else if (worldClient && renderer->switchFrameBuffer("hud")) {
      renderer->clearCurrentFrameBuffer();
      m_mainInterface->setCursorDrawnSeparately(true);
      m_mainInterface->render();
      renderer->switchToDefaultFrameBuffer();
      renderer->compositeFrameBufferToCurrent("hud");
      m_mainInterface->renderCursorOverlay();
      m_hudFbValid = true;
    } else {
      m_mainInterface->setCursorDrawnSeparately(false);
      m_mainInterface->render();
    }
#else
    m_mainInterface->render();
#endif
    m_cinematicOverlay->render();
#ifdef STAR_SYSTEM_SWITCH
    lap(s_ifaceUs);
    joinSimTick();
    lap(s_joinUs);
    s_simUs += m_simLastTickUs;
    m_simLastTickUs = 0;
    // World lifecycle packets the worker deferred are applied here, on the
    // main thread with no frame in flight (nothing holds pointers into the
    // world client's buffers between frames).
    m_universeClient->flushDeferredWorldPackets();
    if (++s_frames >= 150) {
      Logger::info("[perf] fps={:.1f} frame={:.1f}ms snap={:.1f} paint={:.1f} join={:.1f} iface={:.1f} sim={:.1f} other={:.1f}",
          1e6 * s_frames / std::max<int64_t>(s_frameUs, 1),
          s_frameUs / 1e3 / s_frames, s_snapUs / 1e3 / s_frames,
          s_paintUs / 1e3 / s_frames, s_joinUs / 1e3 / s_frames,
          s_ifaceUs / 1e3 / s_frames, s_simUs / 1e3 / s_frames,
          (s_frameUs - s_snapUs - s_paintUs - s_joinUs - s_ifaceUs) / 1e3 / s_frames);
      s_frames = 0;
      s_frameUs = s_snapUs = s_paintUs = s_joinUs = s_ifaceUs = s_simUs = 0;
      // Guest-memory watermark: a slow per-frame heap leak degrades driver
      // performance long before it OOMs, so track committed process memory.
      // newlib heap view: covers BOTH rpmalloc span growth (engine) and direct
      // newlib users (the mesa/nouveau GL driver) -- deltas locate slow leaks.
      struct mallinfo mi = mallinfo();
      Logger::info("[perf-mem] newlibUsed={}kB newlibFree={}kB arena={}kB",
          (uint64_t)(unsigned)mi.uordblks >> 10, (uint64_t)(unsigned)mi.fordblks >> 10,
          (uint64_t)(unsigned)mi.arena >> 10);
      char atBuf[512];
      starAllocTrackReport(atBuf, sizeof(atBuf));
      Logger::info("[perf-alloc]{}", (char const*)atBuf);
    }
#endif
#if !STAR_SYSTEM_ANDROID && !STAR_SYSTEM_IOS
    LogMap::set("client_render_interface", strf(u8"{:05d}\u00b5s", Time::monotonicMicroseconds() - start));
#endif
  }

  if (!m_errorScreen->accepted())
    m_errorScreen->render();

}

void ClientApplication::getAudioData(int16_t* sampleData, size_t frameCount) {
  if (m_mainMixer) {
    m_mainMixer->read(sampleData, frameCount, [&](int16_t* buffer, size_t frames, unsigned channels) {
      if (m_voice)
        m_voice->mix(buffer, frames, channels);
    });
  }
}

auto postProcessGroupsRoot = "postProcessGroups";

void ClientApplication::renderReload() {
  auto assets = m_root->assets();
  auto renderer = Application::renderer();

  auto loadEffectConfig = [&](String const& name) {
    setMobileStartupStatus(strf("Renderer: loading {} effect...", name));
    String path = strf("/rendering/effects/{}.config", name);
    if (assets->assetExists(path)) {
      StringMap<String> shaders;
      auto config = assets->json(path);
      auto shaderConfig = config.getObject("effectShaders");
      for (auto& entry : shaderConfig) {
        if (entry.second.isType(Json::Type::String)) {
          String shader = entry.second.toString();
          if (!shader.hasChar('\n')) {
            auto shaderBytes = assets->bytes(AssetPath::relativeTo(path, shader));
            shader = std::string(shaderBytes->ptr(), shaderBytes->size());
          }
          shaders[entry.first] = shader;
        }
      }

      renderer->loadEffectConfig(name, config, shaders);
    } else
      Logger::warn("No rendering config found for renderer with id '{}'", renderer->rendererId());
  };

  setMobileStartupStatus("Renderer: loading framebuffer configuration...");
  Json openglConfig = assets->json("/rendering/opengl.config");
  // NOTE: a render-scale knob (framebuffers.main.sizeMul) exists and works, but
  // testing at 0.66 and 0.4 showed the in-world render is CPU-bound (primitive
  // building), NOT GPU fill-rate -- only the planet horizon (~6ms) scaled with
  // resolution, so it is left at full res (no quality cost for negligible gain).
#ifdef STAR_SYSTEM_FAMILY_MOBILE
  // The same render-scale testing DID show the environment background (sky
  // gradient, sun + rays, planet horizon, stars, debris) scales ~linearly with
  // resolution -- it's fill-rate, ~19-26ms/frame of the mobile GPU budget in
  // every scene. That content is all low-frequency (gradients, glows, distant
  // sprites), so WorldPainter draws it into this quarter-pixel-count buffer
  // and stretch-blits it under the full-resolution world layers; the upscale
  // is not visually distinguishable for such content, unlike the world layer
  // (crisp pixel-art tiles/entities) which stays full-res.
  {
    // "preserve": WorldPainter refreshes this buffer only every 2nd frame
    // under load and re-blits the previous contents in between, so it must
    // survive across frames (it's fully overdrawn by the sky on refresh).
    auto frameBuffers = openglConfig.get("frameBuffers").set("background",
        JsonObject{{"sizeMul", Json(0.5)}, {"preserve", Json(true)}});
#ifdef STAR_SYSTEM_SWITCH
    // Retained HUD overlay buffer: the full interface is rendered into this
    // buffer only on sync frames (when the sim tick has been joined and live
    // game state reads are safe) and alpha-composited over the scene on every
    // frame in between -- replacing the per-frame re-submission of all UI
    // primitives through the (translated, expensive) guest GL driver with a
    // single textured quad.
    frameBuffers = frameBuffers.set("hud",
        JsonObject{{"preserve", Json(true)}, {"premultiplied", Json(true)}});
#endif
    openglConfig = openglConfig.set("frameBuffers", frameBuffers);
  }
#endif
  renderer->loadConfig(openglConfig);
  
  loadEffectConfig("world");
  
  // define post process groups and set them to be enabled/disabled based on config
  
  auto config = m_root->configuration();
  if (!config->get(postProcessGroupsRoot).isType(Json::Type::Object))
    config->set(postProcessGroupsRoot, JsonObject());
  auto groupsConfig = config->get(postProcessGroupsRoot);
  
  m_postProcessGroups.clear();
  auto postProcessGroups = assets->json("/client.config:postProcessGroups").toObject();
  for (auto& pair : postProcessGroups) {
    auto name = pair.first;
    auto groupConfig = groupsConfig.opt(name);
    auto def = pair.second.getBool("enabledDefault",true);
    if (!groupConfig)
      config->setPath(strf("{}.{}", postProcessGroupsRoot, name),JsonObject());
    m_postProcessGroups.add(name,PostProcessGroup{ groupConfig ? groupConfig.value().getBool("enabled", def) : def });
  }
  
  // define post process layers and optionally assign them to groups
  m_postProcessLayers.clear();
  m_labelledPostProcessLayers.clear();
  auto postProcessLayers = assets->json("/client.config:postProcessLayers").toArray();
  for (auto& layer : postProcessLayers) {
    auto effects = jsonToStringList(layer.getArray("effects"));
    for (auto& effect : effects)
      loadEffectConfig(effect);
    PostProcessGroup* group = nullptr;
    auto gname = layer.optString("group");
    if (gname) {
      group = &m_postProcessGroups.get(gname.value());
    }
    // I'd think a string map for all of these would be better, but the order does matter here, and does make sense to depend on mod priority, so...
    // guess a string map of indices works
    // I tried pointers but for whatever reason the behaviour was highly inconsistent and only worked after reload...
    auto label = layer.optString("name");
    if (label) {
      m_labelledPostProcessLayers.add(label.value(),m_postProcessLayers.count());
    }
    m_postProcessLayers.append(PostProcessLayer{ std::move(effects), (unsigned)layer.getUInt("passes", 1), group });
  }

  loadEffectConfig("interface");
}

void ClientApplication::setPostProcessLayerPasses(String const& layer, unsigned const& passes) {
  m_postProcessLayers.at(m_labelledPostProcessLayers.get(layer)).passes = passes;
}
void ClientApplication::setPostProcessGroupEnabled(String const& group, bool const& enabled, Maybe<bool> const& save) {
  m_postProcessGroups.get(group).enabled = enabled;
  if (save && save.value())
    m_root->configuration()->setPath(strf("{}.{}.enabled", postProcessGroupsRoot, group),enabled);
}
bool ClientApplication::postProcessGroupEnabled(String const& group) {
  return m_postProcessGroups.get(group).enabled;
}

Json ClientApplication::postProcessGroups() {
  return m_root->assets()->json("/client.config:postProcessGroups");
}

unsigned ClientApplication::framesSkipped() const {
  return m_framesSkipped;
}

void ClientApplication::changeState(MainAppState newState) {
  MainAppState oldState = m_state;
  m_state = newState;
  auto& app = appController();

  if (m_state == MainAppState::Quit)
    app->quit();

  if (newState == MainAppState::Mods)
    m_cinematicOverlay->load(m_root->assets()->json("/cinematics/mods/modloading.cinematic"));

  if (newState == MainAppState::Splash) {
    m_cinematicOverlay->load(m_root->assets()->json("/cinematics/splash.cinematic"));
    m_rootLoader = Thread::invoke("Async root loader", [this]() {
        m_root->fullyLoad();
      });
  }

  if (oldState > MainAppState::Title && m_state <= MainAppState::Title) {
    if (m_universeClient)
      m_universeClient->disconnect();

    if (m_universeServer) {
      m_universeServer->stop();
      m_universeServer->join();
      m_universeServer.reset();
    }
    m_cinematicOverlay->stop();

    // Clear HTTP trust request callback
    LuaBindings::clearHttpTrustRequestCallback();

    m_mainInterface.reset();

    if (m_voice)
      m_voice->clearSpeakers();

    if (auto p2pNetworkingService = app->p2pNetworkingService()) {
      p2pNetworkingService->setJoinUnavailable();
      p2pNetworkingService->setAcceptingP2PConnections(false);
    }
  }

  if (oldState > MainAppState::Title && m_state == MainAppState::Title) {
    m_titleScreen->resetState();
    m_mainMixer->setUniverseClient({});
  }
  if (oldState >= MainAppState::Title && m_state < MainAppState::Title) {
    m_playerStorage.reset();

    if (m_statistics) {
      m_statistics->writeStatistics();
      m_statistics.reset();
    }

    m_universeClient.reset();
    m_mainMixer->setUniverseClient({});
    m_titleScreen.reset();
  }

  if (oldState < MainAppState::Title && m_state >= MainAppState::Title) {
    if (m_rootLoader)
      m_rootLoader.finish();

    m_cinematicOverlay->stop();

    m_playerStorage = make_shared<PlayerStorage>(m_root->toStoragePath("player"));
    m_statistics = make_shared<Statistics>(m_root->toStoragePath("player"), app->statisticsService());
    m_universeClient = make_shared<UniverseClient>(m_playerStorage, m_statistics);

    m_universeClient->setLuaCallbacks("input", LuaBindings::makeInputCallbacks());
    m_universeClient->setLuaCallbacks("voice", LuaBindings::makeVoiceCallbacks());
    m_universeClient->setLuaCallbacks("camera", LuaBindings::makeCameraCallbacks(&m_worldPainter->camera()));
    m_universeClient->setLuaCallbacks("renderer", LuaBindings::makeRenderingCallbacks(this));

    Json alwaysAllow = m_root->configuration()->getPath("safe.alwaysAllowClipboard");
    m_universeClient->setLuaCallbacks("clipboard", LuaBindings::makeClipboardCallbacks(app, alwaysAllow && alwaysAllow.toBool()));
    const bool luaHttpEnabled = m_root->configuration()->getPath("safe.luaHttp.enabled").optBool().value(false);

    m_universeClient->setLuaCallbacks("http", LuaBindings::makeHttpCallbacks(luaHttpEnabled));

    auto heldScriptPanes = make_shared<List<MainInterface::ScriptPaneInfo>>();

    m_universeClient->playerReloadPreCallback() = [&, heldScriptPanes](bool resetInterface) {
      if (!resetInterface)
        return;

      m_mainInterface->takeScriptPanes(*heldScriptPanes);
    };

    m_universeClient->playerReloadCallback() = [&, heldScriptPanes](bool resetInterface) {
      auto paneManager = m_mainInterface->paneManager();
      if (auto inventory = paneManager->registeredPane<InventoryPane>(MainInterfacePanes::Inventory))
        inventory->clearChangedSlots();

      if (resetInterface) {
        m_mainInterface->reviveScriptPanes(*heldScriptPanes);
        heldScriptPanes->clear();
      }
    };

    m_mainMixer->setUniverseClient(m_universeClient);
    m_titleScreen = make_shared<TitleScreen>(m_playerStorage, m_mainMixer->mixer(), m_universeClient);
    if (auto renderer = Application::renderer())
      m_titleScreen->renderInit(renderer);
  }

  if (m_state == MainAppState::Title) {
    auto configuration = m_root->configuration();

    if (m_pendingMultiPlayerConnection) {
      if (auto address = m_pendingMultiPlayerConnection->server.ptr<HostAddressWithPort>()) {
        m_titleScreen->setMultiPlayerAddress(toString(address->address()));
        m_titleScreen->setMultiPlayerPort(toString(address->port()));
        m_titleScreen->setMultiPlayerAccount(configuration->getPath("title.multiPlayerAccount").toString());
        m_titleScreen->setMultiPlayerForceLegacy(configuration->getPath("title.multiPlayerForceLegacy").optBool().value(false));
        m_titleScreen->goToMultiPlayerSelectCharacter(false);
      } else {
        m_titleScreen->goToMultiPlayerSelectCharacter(true);
      }
    } else {
      m_titleScreen->setMultiPlayerAddress(configuration->getPath("title.multiPlayerAddress").toString());
      m_titleScreen->setMultiPlayerPort(configuration->getPath("title.multiPlayerPort").toString());
      m_titleScreen->setMultiPlayerAccount(configuration->getPath("title.multiPlayerAccount").toString());
      m_titleScreen->setMultiPlayerForceLegacy(configuration->getPath("title.multiPlayerForceLegacy").optBool().value(false));
    }
  }

  if (m_state > MainAppState::Title) {
    if (m_titleScreen->currentlySelectedPlayer()) {
      m_player = m_titleScreen->currentlySelectedPlayer();
    } else {
      if (auto uuid = m_playerStorage->playerUuidAt(0))
        m_player = m_playerStorage->loadPlayer(*uuid);

      if (!m_player) {
        setError("Error loading player!");
        return;
      }
    }

    m_mainMixer->setUniverseClient(m_universeClient);
    m_universeClient->setMainPlayer(m_player);
    m_cinematicOverlay->setPlayer(m_player);
    m_timeSinceJoin = (int64_t)Time::millisecondsSinceEpoch() / 1000;

    auto assets = m_root->assets();
    String loadingCinematic = assets->json("/client.config:loadingCinematic").toString();
    m_cinematicOverlay->load(assets->json(loadingCinematic));
    if (!m_player->log()->introComplete()) {
      String introCinematic = assets->json("/client.config:introCinematic").toString();
      introCinematic = introCinematic.replaceTags(StringMap<String>{{"species", m_player->species()}});
      m_player->setPendingCinematic(Json(introCinematic));
    } else {
      m_player->setPendingCinematic(Json());
    }

    if (m_state == MainAppState::MultiPlayer) {
      PacketSocketUPtr packetSocket;

      auto multiPlayerConnection = m_pendingMultiPlayerConnection.take();

      if (auto address = multiPlayerConnection.server.ptr<HostAddressWithPort>()) {
        try {
          packetSocket = TcpPacketSocket::open(TcpSocket::connectTo(*address));
        } catch (StarException const& e) {
          setError(strf("Join failed! Error connecting to '{}'", *address), e);
          return;
        }

      } else {
        auto p2pPeerId = multiPlayerConnection.server.ptr<P2PNetworkingPeerId>();

        if (auto p2pNetworkingService = app->p2pNetworkingService()) {
          auto result = p2pNetworkingService->connectToPeer(*p2pPeerId);
          if (result.isLeft()) {
            setError(strf("Cannot join peer: {}", result.left()));
            return;
          } else {
            packetSocket = P2PPacketSocket::open(std::move(result.right()));
          }
        } else {
          setError("Internal error, no p2p networking service when joining p2p networking peer");
          return;
        }
      }

      bool allowAssetsMismatch = m_root->configuration()->get("allowAssetsMismatch").toBool();
      if (auto errorMessage = m_universeClient->connect(UniverseConnection(std::move(packetSocket)), allowAssetsMismatch,
            multiPlayerConnection.account, multiPlayerConnection.password, multiPlayerConnection.forceLegacy)) {
        setError(*errorMessage);
        return;
      }

      if (auto address = multiPlayerConnection.server.ptr<HostAddressWithPort>())
        m_currentRemoteJoin = *address;
      else
        m_currentRemoteJoin.reset();

    } else {
      if (!m_universeServer) {
        try {
          m_universeServer = make_shared<UniverseServer>(m_root->toStoragePath("universe"));
          m_universeServer->start();
        } catch (StarException const& e) {
          setError("Unable to start local server", e);
          return;
        }
      }

      Maybe<String> localConnectError;
      static String const MissingProtocolResponse = "Expected ProtocolResponse, but none received";
      for (int attempt = 1; attempt <= 5; ++attempt) {
        localConnectError = m_universeClient->connect(m_universeServer->addLocalClient(), "", "");
        if (!localConnectError)
          break;

        Logger::warn("ClientApplication: local connection attempt {} failed: {}", attempt, *localConnectError);
        if (!localConnectError->contains(MissingProtocolResponse))
          break;

        Thread::sleep((unsigned)(200 * attempt));
      }

      if (localConnectError) {
        setError(strf("Error connecting locally: {}", *localConnectError));
        return;
      }
    }

    m_titleScreen->stopMusic();

    m_universeClient->restartLua();
    m_mainInterface = make_shared<MainInterface>(m_universeClient, m_worldPainter, m_cinematicOverlay);
    m_universeClient->setLuaCallbacks("interface", LuaBindings::makeInterfaceCallbacks(m_mainInterface.get()));
    m_universeClient->setLuaCallbacks("chat", LuaBindings::makeChatCallbacks(m_mainInterface.get(), m_universeClient.get()));
    m_universeClient->setLuaCallbacks("celestial", LuaBindings::makeCelestialCallbacks(m_universeClient.get()));
    m_universeClient->setLuaCallbacks("team", LuaBindings::makeTeamClientCallbacks(m_universeClient->teamClient().get()));
    m_universeClient->setLuaCallbacks("world", LuaBindings::makeWorldCallbacks(m_universeClient->worldClient().get()));

    LuaBindings::setHttpTrustRequestCallback([mainInterface = m_mainInterface.get()](String const& domain) {
      const auto paneManager = mainInterface->paneManager();
      const auto httpTrustDialog = paneManager->registeredPane<HttpTrustDialog>(MainInterfacePanes::HttpTrustDialog);

      httpTrustDialog->displayRequest(domain, [domain](const HttpTrustReply reply, bool remember) {
        const bool allowed = (reply == HttpTrustReply::Allow);
        LuaBindings::handleHttpTrustReply(domain, allowed);
      });
      paneManager->displayRegisteredPane(MainInterfacePanes::HttpTrustDialog);
    });


    m_mainInterface->displayDefaultPanes();
    m_universeClient->startLuaScripts();

    m_mainMixer->setWorldPainter(m_worldPainter);

    if (auto renderer = Application::renderer()) {
      m_worldPainter->renderInit(renderer);
    }
  }
}

void ClientApplication::setError(String const& error) {
  Logger::error(error.utf8Ptr());
  m_errorScreen->setMessage(error);
  m_titleScreen->resetState();
  changeState(MainAppState::Title);
}

void ClientApplication::setError(String const& error, std::exception const& e) {
  Logger::error("{}\n{}", error, outputException(e, true));
  m_errorScreen->setMessage(strf("{}\n{}", error, outputException(e, false)));
  m_titleScreen->resetState();
  changeState(MainAppState::Title);
}

void ClientApplication::loadMods() {
  auto ugcService = appController()->userGeneratedContentService();
  auto configuration = m_root->configuration();
  bool includeUGC = configuration->get("includeUGC", m_root->settings().includeUGC).toBool();
  if (ugcService && includeUGC) {
    StringList modDirectories;
    Logger::info("Checking for user generated content...");
    for (auto& contentId : ugcService->subscribedContentIds()) {
      if (auto contentDirectory = ugcService->contentDownloadDirectory(contentId)) {
        Logger::info("Loading mods from user generated content with id '{}' from directory '{}'", contentId, *contentDirectory);
        modDirectories.append(*contentDirectory);
      } else {
        Logger::warn("User generated content with id '{}' is not available", contentId);
      }
    }

    if (modDirectories.empty()) {
      Logger::info("No subscribed user generated content");
    } else {
      Root::singleton().loadMods(modDirectories, false);
      auto assets = m_root->assets();
    }
  }
}

void ClientApplication::updateSteamFlatpakWarning(float) {
  if (m_errorScreen->accepted())
    changeState(MainAppState::Mods);
}

void ClientApplication::updateMods(float dt) {
  m_cinematicOverlay->update(dt);
  auto ugcService = appController()->userGeneratedContentService();
  auto configuration = m_root->configuration();
  bool includeUGC = configuration->get("includeUGC", m_root->settings().includeUGC).toBool();
  if (ugcService && includeUGC) {
    // Prevent unnecessary log spam when UGC needs to be downloaded
    if (!m_loggedUGCCheck) {
      Logger::info("Checking for user generated content updates...");
      m_loggedUGCCheck = true;
    }
    
    if (ugcService->triggerContentDownload() == UserGeneratedContentService::UGCState::NoDownload) {
      changeState(MainAppState::Splash);
    } else {
      if (ugcService->triggerContentDownload() == UserGeneratedContentService::UGCState::Finished) {
        Logger::info("Loading updated user generated content...");
        StringList modDirectories;
        for (auto& contentId : ugcService->subscribedContentIds()) {
          if (auto contentDirectory = ugcService->contentDownloadDirectory(contentId)) {
            Logger::info("Loading mods from user generated content with id '{}' from directory '{}'", contentId, *contentDirectory);
            modDirectories.append(*contentDirectory);
          } else {
            Logger::warn("User generated content with id '{}' is not available", contentId);
          }
        }

        if (modDirectories.empty()) {
          changeState(MainAppState::Splash);
        } else {
          Logger::info("Reloading to include updated user generated content");
          Root::singleton().loadMods(modDirectories);

          // We've just reloaded, so make sure to grab our config again!
          // If we don't do this, we'll be able to read modsWarningShown
          // just fine, but we won't be able to write it back to the file.
          configuration = m_root->configuration();
        }

        auto assets = m_root->assets();

        if (configuration->get("modsWarningShown").optBool().value()) {
          changeState(MainAppState::Splash);
        } else {
          configuration->set("modsWarningShown", true);
          m_errorScreen->setMessage(assets->json("/interface.config:modsWarningMessage").toString());
          changeState(MainAppState::ModsWarning);
        }
      }
    }
  } else {
    changeState(MainAppState::Splash);
  }
}

void ClientApplication::updateModsWarning(float) {
  if (m_errorScreen->accepted())
    changeState(MainAppState::Splash);
}

void ClientApplication::updateSplash(float dt) {
  m_cinematicOverlay->update(dt);
  if (!m_rootLoader.isRunning()) {
    if (m_rootLoader) {
      try {
        m_rootLoader.finish();
      } catch (std::exception const& e) {
        setError("Error loading game assets!", e);
        return;
      } catch (...) {
        setError("Unknown error loading game assets!");
        return;
      }
    }
  }

  if (!m_rootLoader.isRunning() && (m_cinematicOverlay->completable() || m_cinematicOverlay->completed()))
    changeState(MainAppState::Title);
}

void ClientApplication::updateError(float) {
  if (m_errorScreen->accepted())
    changeState(MainAppState::Title);
}

void ClientApplication::updateTitle(float dt) {
  m_cinematicOverlay->update(dt);

  m_titleScreen->update(dt);
  m_mainMixer->update(dt);
  m_mainMixer->setSpeed(GlobalTimescale);

  auto& app = appController();
  bool inputActive = m_titleScreen->textInputActive();
#ifdef STAR_SYSTEM_SWITCH
  runSwitchKeyboardSession(m_titleScreen->paneManager(), inputActive);
  inputActive = m_titleScreen->textInputActive();
#endif
  if (m_input)
    m_input->setTextInputActive(inputActive);
  if (inputActive)
    app->setTextArea(m_titleScreen->paneManager()->keyboardCapturedWidget()->keyboardCaptureArea());
  else
    app->setTextArea();
  app->setAcceptingTextInput(inputActive);

  auto p2pNetworkingService = app->p2pNetworkingService();
  if (p2pNetworkingService) {
    auto getStateString = [](TitleState state) -> const char* {
      switch (state) {
        case TitleState::Main:
          return "In Main Menu";
        case TitleState::Options:
          return "In Options";
        case TitleState::Mods:
          return "In Mods";
        case TitleState::SinglePlayerSelectCharacter:
          return "Selecting a character for singleplayer";
        case TitleState::SinglePlayerCreateCharacter:
          return "Creating a character for singleplayer";
        case TitleState::MultiPlayerSelectCharacter:
          return "Selecting a character for multiplayer";
        case TitleState::MultiPlayerCreateCharacter:
          return "Creating a character for multiplayer";
        case TitleState::MultiPlayerConnect:
          return "Awaiting multiplayer connection info";
        case TitleState::StartSinglePlayer:
          return "Loading Singleplayer";
        case TitleState::StartMultiPlayer:
          return "Connecting to Multiplayer";
        default:
          return "";
      }
    };

    p2pNetworkingService->setActivityData("Not In Game", getStateString(m_titleScreen->currentState()), 0, {});
  }

#ifdef STAR_PLATFORM_MOBILE
  // Test autopilot: auto-start single player with the first stored character
  // (skips manual title-screen navigation), for measuring perf/jitter without
  // manual input. Enabled by the Switch autopilot.flag or, on other oSBM
  // builds, the STAR_AUTOPILOT environment variable. Absent for normal users.
  // flag/env contents select behavior (empty = beam-down-and-walk): "stay"
  // idles on the ship, "warp=<action>" warps once to a parseWarpAction target.
  {
    static bool s_autopilotStarted = false;
    // Cache the flag check once at title time: on Switch File::isFile is an
    // opendir scan that races libnx fsdev if polled every tick; and the flag
    // doesn't change mid-run anyway.
    static int s_autopilotFlagCached = -1;
    if (s_autopilotFlagCached < 0 && m_titleScreen->currentState() == TitleState::Main) {
      Maybe<String> contents;
#ifdef STAR_SYSTEM_SWITCH
      if (File::isFile("/switch/oSBM/autopilot.flag")) {
        try { contents = String(File::readFileString("/switch/oSBM/autopilot.flag")); }
        catch (std::exception const&) { contents = String(); }
      }
#else
      if (auto env = getenv("STAR_AUTOPILOT"))
        contents = String(env);
#endif
      s_autopilotFlagCached = contents ? 1 : 0;
      if (contents) {
        m_autopilotStayOnShip = contents->contains("stay");
        for (auto const& line : contents->split('\n')) {
          auto trimmed = line.trim();
          if (trimmed.beginsWith("warp="))
            m_autopilotWarpTarget = trimmed.substr(5).trim();
        }
      }
    }
    if (!s_autopilotStarted && m_titleScreen->currentState() == TitleState::Main
        && m_playerStorage && m_playerStorage->playerUuidAt(0)
        && s_autopilotFlagCached == 1) {
      s_autopilotStarted = true;
      m_autopilotActive = true;
      changeState(MainAppState::SinglePlayer);
    }
  }
#endif

  if (m_titleScreen->currentState() == TitleState::StartSinglePlayer) {
    changeState(MainAppState::SinglePlayer);

  } else if (m_titleScreen->currentState() == TitleState::StartMultiPlayer) {
    if (!m_pendingMultiPlayerConnection || m_pendingMultiPlayerConnection->server.is<HostAddressWithPort>()) {
      auto addressString = m_titleScreen->multiPlayerAddress().trim();
      auto portString = m_titleScreen->multiPlayerPort().trim();
      portString = portString.empty() ? toString(m_root->configuration()->get("gameServerPort").toUInt()) : portString;
      if (auto port = maybeLexicalCast<uint16_t>(portString)) {
        auto address = HostAddressWithPort::lookup(addressString, *port);
        if (address.isLeft()) {
          setError(address.left());
        } else {
          m_pendingMultiPlayerConnection = PendingMultiPlayerConnection{
            address.right(),
            m_titleScreen->multiPlayerAccount(),
            m_titleScreen->multiPlayerPassword(),
            m_titleScreen->multiPlayerForceLegacy()
          };

          auto configuration = m_root->configuration();
          configuration->setPath("title.multiPlayerAddress", m_titleScreen->multiPlayerAddress());
          configuration->setPath("title.multiPlayerPort", m_titleScreen->multiPlayerPort());
          configuration->setPath("title.multiPlayerAccount", m_titleScreen->multiPlayerAccount());
          configuration->setPath("title.multiPlayerForceLegacy", m_titleScreen->multiPlayerForceLegacy());

          changeState(MainAppState::MultiPlayer);
        }
      } else {
        setError(strf("invalid port: {}", portString));
      }
    } else {
      changeState(MainAppState::MultiPlayer);
    }

  } else if (m_titleScreen->currentState() == TitleState::Quit) {
    changeState(MainAppState::Quit);
  }
}

void ClientApplication::updateRunning(float dt) {
  try {
    auto& app = appController();
    auto worldClient = m_universeClient->worldClient();

#ifdef STAR_SYSTEM_SWITCH
    // Catch-up frames call update() more than once before the next render():
    // a sim tick deferred by the previous update must run NOW, before this
    // tick's input handling below applies fresh player controls -- running it
    // any later would consume (and clear) THIS tick's controls one tick
    // early, so the deferred tick would see jump/move released for a tick.
    // On real hardware that manifested as held-jump becoming repeated tiny
    // hops and walk-speed stutter on every frame hitch.
    if (m_simTickPending) {
      m_simTickPending = false;
      m_universeClient->update(m_simTickDt);
    }
#endif

#ifdef STAR_PLATFORM_MOBILE
    // Autopilot perf-testing: once the ship has arrived at the starter world, auto-warp
    // to the planet surface so the real gameplay scene can be measured (the ship/space
    // hub is not where players spend their time). This bypasses canBeamDown()'s UI-only
    // gates (WarpMode::DeployOnly needing a mech via canDeploy(), teleport-capability
    // checks, etc.) and warps directly via the same WarpAlias::OrbitedWorld the UI
    // buttons use -- a fresh tutorial character has no mech and no teleport capability
    // yet, so canBeamDown() never returns true for them, but the server resolves
    // OrbitedWorld from orbitWarpAction() regardless of those UI-only checks.
    {
      // Re-armable (not one-shot): if the player ends up back on the SHIP
      // (e.g. died on the surface and respawned) during an unattended soak
      // run, beam down again after a cooldown so the run keeps measuring the
      // planet-surface scene instead of idling in orbit forever.
      static double s_lastAutoBeam = 0;
      double nowSecs = Time::monotonicTime();
      // Cheap in-memory checks first: File::isFile is an emulated fs stat on
      // Switch and this runs every update tick once the cooldown has expired.
      // The flag state is cached at title time (see updateTitle): repeated
      // File::isFile from the main thread races libnx fsdev (see there).
      if (m_mainInterface && (nowSecs - s_lastAutoBeam > 60.0)
          && m_universeClient->playerWorld().is<ClientShipWorldId>()
          && !m_universeClient->flying() && m_universeClient->clientContext()->orbitWarpAction()
          && m_autopilotActive && !m_autopilotStayOnShip && m_autopilotWarpTarget.empty()) {
        s_lastAutoBeam = nowSecs;
        m_universeClient->warpPlayer(WarpAlias::OrbitedWorld, true, "beam");
      }

      // Autopilot warp target: one-shot warp to an arbitrary destination
      // (e.g. warp=instanceworld:outpost for the #39 parallax scene) once the
      // player is settled in their arrival world.
      static bool s_autopilotWarpDone = false;
      static double s_autopilotWarpArmAt = 0;
      if (m_autopilotActive && !s_autopilotWarpDone && !m_autopilotWarpTarget.empty()
          && m_player && m_player->inWorld()) {
        if (s_autopilotWarpArmAt == 0)
          s_autopilotWarpArmAt = nowSecs + 10.0;
        else if (nowSecs > s_autopilotWarpArmAt) {
          s_autopilotWarpDone = true;
          try {
            m_universeClient->warpPlayer(parseWarpAction(m_autopilotWarpTarget), true, "beam");
            Logger::info("[autopilot] warping to {}", m_autopilotWarpTarget);
          } catch (std::exception const& e) {
            Logger::error("[autopilot] warp to '{}' failed: {}", m_autopilotWarpTarget, e.what());
          }
        }
      }

      // Autopilot walk: exercise REAL gameplay load (camera scroll, tile
      // chunk rebuilds, lighting recalculation, sector loads, humanoid
      // animation) instead of an idle stare.  Walk one direction for a
      // while, hop occasionally, then turn around; net drift stays bounded
      // around the beam-down point.
      if (m_autopilotActive && m_player && m_player->inWorld()
          && m_universeClient->playerWorld().is<CelestialWorldId>()) {
        static uint64_t s_walkTick = 0;
        ++s_walkTick;
        // Live A/B lever: drop autopilot-stand.flag on the sd card to halt the
        // walk mid-run (isolates content-churn load from pure elapsed time).
        static bool s_standFlag = false;
        if (s_walkTick % 512 == 1)
          s_standFlag = File::isFile("/switch/oSBM/autopilot-stand.flag");
        if (!s_standFlag) {
          // Walk continuously in one direction: real exploration constantly
          // enters new sectors (generation, microdungeon placement, chunk and
          // lighting churn) -- the load an idle or bounded soak never shows.
          m_player->moveRight();
          if (s_walkTick % 90 == 0) // hop every ~3s to clear obstacles
            m_player->jump();
        }
      }
    }
#endif
    auto p2pNetworkingService = app->p2pNetworkingService();
#ifdef STAR_SYSTEM_SWITCH
    // Configuration::get is a mutex + Json map lookup; these two settings
    // change only via the options menu, so refresh them at ~2Hz instead of
    // paying the lookup every sim tick.
    static uint64_t s_joinableRefreshCounter = 0;
    static bool s_clientIPJoinable = false, s_clientP2PJoinable = false;
    if (s_joinableRefreshCounter++ % 32 == 0) {
      s_clientIPJoinable = m_root->configuration()->get("clientIPJoinable").toBool();
      s_clientP2PJoinable = m_root->configuration()->get("clientP2PJoinable").toBool();
    }
    bool clientIPJoinable = s_clientIPJoinable;
    bool clientP2PJoinable = s_clientP2PJoinable;
#else
    bool clientIPJoinable = m_root->configuration()->get("clientIPJoinable").toBool();
    bool clientP2PJoinable = m_root->configuration()->get("clientP2PJoinable").toBool();
#endif
    Maybe<pair<uint16_t, uint16_t>> party = make_pair(m_universeClient->players(), m_universeClient->maxPlayers());

    if (m_state == MainAppState::MultiPlayer) {
      if (p2pNetworkingService) {
        p2pNetworkingService->setAcceptingP2PConnections(false);
        if (clientP2PJoinable && m_currentRemoteJoin)
          p2pNetworkingService->setJoinRemote(*m_currentRemoteJoin);
        else
          p2pNetworkingService->setJoinUnavailable();
      }
    } else {
      m_universeServer->setListeningTcp(clientIPJoinable);
      if (p2pNetworkingService) {
        p2pNetworkingService->setAcceptingP2PConnections(clientP2PJoinable);
        if (clientP2PJoinable) {
          p2pNetworkingService->setJoinLocal(m_universeServer->maxClients());
        } else {
          p2pNetworkingService->setJoinUnavailable();
          party = {};
        }
      }
    }
    
    if (p2pNetworkingService) {
      auto getActivityDetail = [&](String const& tag) -> String {
        if (tag == "playerName")
          return Text::stripEscapeCodes(m_player->name());
        if (tag == "playerHealth")
          return toString(m_player->health());
        if (tag == "playerMaxHealth")
          return toString(m_player->maxHealth());
        if (tag == "playerEnergy")
          return toString(m_player->energy());
        if (tag == "playerMaxEnergy")
          return toString(m_player->maxEnergy());
        if (tag == "playerBreath")
          return toString(m_player->breath());
        if (tag == "playerMaxBreath")
          return toString(m_player->maxBreath());
        if (tag == "playerXPos")
          return toString(round(m_player->position().x()));
        if (tag == "playerYPos")
          return toString(round(m_player->position().y()));
        if (tag == "worldName") {
          if (m_universeClient->clientContext()->playerWorldId().is<ClientShipWorldId>())
            return "Player Ship";
          else if (WorldTemplate const* worldTemplate = worldClient ? worldClient->currentTemplate().get() : nullptr) {
            auto worldName = worldTemplate->worldName();
            if (worldName.empty())
              return "In World";
            else
              return Text::stripEscapeCodes(worldName);
          }
          else
            return "Nowhere";
        }
        return "";
      };

      String finalDetails = "";
      Json activityDetails = m_root->configuration()->getPath("discord.activityDetails");
      if (activityDetails.isType(Json::Type::Array)) {
        StringList detailsList;
        for (auto& detail : activityDetails.iterateArray())
          detailsList.append(getActivityDetail(*detail.stringPtr()));
        finalDetails = detailsList.join("\n");
      } else if (activityDetails.isType(Json::Type::String))
        finalDetails = activityDetails.toString().lookupTags(getActivityDetail);

      p2pNetworkingService->setActivityData("In Game", finalDetails.utf8Ptr(), m_timeSinceJoin, party);
    }

    bool allowPlayerInput = false;
#if STAR_SYSTEM_ANDROID || STAR_SYSTEM_IOS
    allowPlayerInput = !m_mainInterface->inputFocus();
#else
    allowPlayerInput = !m_cinematicOverlay->suppressInput() && !m_mainInterface->inputFocus();
#endif
#if STAR_SYSTEM_ANDROID || STAR_SYSTEM_IOS
    if (!allowPlayerInput) {
      allowPlayerInput =
          isActionTaken(InterfaceAction::PlayerRight)
          || isActionTaken(InterfaceAction::PlayerLeft)
          || isActionTaken(InterfaceAction::PlayerUp)
          || isActionTaken(InterfaceAction::PlayerDown)
          || isActionTaken(InterfaceAction::PlayerJump)
          || isActionTakenEdge(InterfaceAction::PlayerJump)
          || isActionTaken(InterfaceAction::PlayerInteract)
          || isActionTakenEdge(InterfaceAction::PlayerInteract);
    }
#endif

    if (allowPlayerInput) {
      m_player->setShifting(isActionTaken(InterfaceAction::PlayerShifting));

      if (isActionTaken(InterfaceAction::PlayerRight))
        m_player->moveRight();
      if (isActionTaken(InterfaceAction::PlayerLeft))
        m_player->moveLeft();
      if (isActionTaken(InterfaceAction::PlayerUp))
        m_player->moveUp();
      if (isActionTaken(InterfaceAction::PlayerDown))
        m_player->moveDown();
      if (isActionTaken(InterfaceAction::PlayerJump) || isActionTakenEdge(InterfaceAction::PlayerJump))
        m_player->jump();

      if (isActionTaken(InterfaceAction::PlayerTechAction1))
        m_player->special(1);
      if (isActionTaken(InterfaceAction::PlayerTechAction2))
        m_player->special(2);
      if (isActionTaken(InterfaceAction::PlayerTechAction3))
        m_player->special(3);

      if (isActionTakenEdge(InterfaceAction::PlayerInteract))
        m_player->beginTrigger();
      else if (!isActionTaken(InterfaceAction::PlayerInteract))
        m_player->endTrigger();

      if (isActionTakenEdge(InterfaceAction::PlayerDropItem))
        m_player->dropItem();

      if (isActionTakenEdge(InterfaceAction::EmoteBlabbering))
        m_player->addEmote(HumanoidEmote::Blabbering);
      if (isActionTakenEdge(InterfaceAction::EmoteShouting))
        m_player->addEmote(HumanoidEmote::Shouting);
      if (isActionTakenEdge(InterfaceAction::EmoteHappy))
        m_player->addEmote(HumanoidEmote::Happy);
      if (isActionTakenEdge(InterfaceAction::EmoteSad))
        m_player->addEmote(HumanoidEmote::Sad);
      if (isActionTakenEdge(InterfaceAction::EmoteNeutral))
        m_player->addEmote(HumanoidEmote::NEUTRAL);
      if (isActionTakenEdge(InterfaceAction::EmoteLaugh))
        m_player->addEmote(HumanoidEmote::Laugh);
      if (isActionTakenEdge(InterfaceAction::EmoteAnnoyed))
        m_player->addEmote(HumanoidEmote::Annoyed);
      if (isActionTakenEdge(InterfaceAction::EmoteOh))
        m_player->addEmote(HumanoidEmote::Oh);
      if (isActionTakenEdge(InterfaceAction::EmoteOooh))
        m_player->addEmote(HumanoidEmote::OOOH);
      if (isActionTakenEdge(InterfaceAction::EmoteBlink))
        m_player->addEmote(HumanoidEmote::Blink);
      if (isActionTakenEdge(InterfaceAction::EmoteWink))
        m_player->addEmote(HumanoidEmote::Wink);
      if (isActionTakenEdge(InterfaceAction::EmoteEat))
        m_player->addEmote(HumanoidEmote::Eat);
      if (isActionTakenEdge(InterfaceAction::EmoteSleep))
        m_player->addEmote(HumanoidEmote::Sleep);

      if (m_input && (int)m_input->bindHeld("opensb", "zoomIn") - (int)m_input->bindHeld("opensb", "zoomOut"))
      {
        int newZoomDirection = (int)m_input->bindHeld("opensb", "zoomIn") - (int)m_input->bindHeld("opensb", "zoomOut");
        m_cameraZoomDirection = newZoomDirection;
      }
    }
    if (m_cameraZoomDirection != 0) {
      const float threshold = 0.01f;
      bool goingIn = m_cameraZoomDirection == 1;
      auto config = m_root->configuration();
      float curZoom = config->get("zoomLevel").toFloat(),
            newZoom = max(1.f, curZoom * powf(1.f + (float)m_cameraZoomDirection * 0.5f, min(1.f, dt * 5.f))),
            intZoom = max(1.f, (goingIn ? floor(curZoom) : ceil(curZoom)) + m_cameraZoomDirection);
      bool pastInt = goingIn ? newZoom + threshold > intZoom
                             : newZoom - threshold < intZoom;
      if (pastInt) {
        float intNewZoom = goingIn ? ceil(newZoom) : floor(newZoom);
        newZoom = lerp(clamp(abs(intZoom - newZoom) - 1.f, 0.f, 1.f), intZoom, intNewZoom);
        m_cameraZoomDirection = 0;
      }
      config->set("zoomLevel", min(1000000.f, newZoom));
    }

#if STAR_SYSTEM_ANDROID || STAR_SYSTEM_IOS
    bool virtualDirectionHeld =
        isActionTaken(InterfaceAction::PlayerRight) ||
        isActionTaken(InterfaceAction::PlayerLeft);

    if (!virtualDirectionHeld && m_controllerInput && m_controllerLeftStick.magnitudeSquared() > 0.01f)
      m_player->setMoveVector(m_controllerLeftStick);
    else
      m_player->setMoveVector(Vec2F());
#else
    if (m_controllerInput && m_controllerLeftStick.magnitudeSquared() > 0.01f)
      m_player->setMoveVector(m_controllerLeftStick);
    else
      m_player->setMoveVector(Vec2F());
#endif

    if (m_voice && m_input)
      m_voice->setInput(m_input->bindHeld("opensb", "pushToTalk"));
    DataStreamBuffer voiceData;
    voiceData.setByteOrder(ByteOrder::LittleEndian);
    //voiceData.writeBytes(VoiceBroadcastPrefix.utf8Bytes()); transmitting with SE compat for now
    bool needstoSendVoice = m_voice ? m_voice->send(voiceData, 5000) : false;

    auto checkDisconnection = [this]() {
      if (!m_universeClient->isConnected()) {
        m_cinematicOverlay->stop();
        String errMessage;
        if (auto disconnectReason = m_universeClient->disconnectReason())
          errMessage = strf("You were disconnected from the server for the following reason:\n{}", *disconnectReason);
        else
          errMessage = "Client-server connection no longer valid!";
        setError(errMessage);
        changeState(MainAppState::Title);
        return true;
      }

      return false;
    };

    if (checkDisconnection())
      return;

    m_mainInterface->preUpdate(dt);
#ifdef STAR_SYSTEM_SWITCH
    // Safety net for frames that never reach the world render path (state
    // changes, menus): worker-deferred world lifecycle packets must still be
    // applied promptly, and this is a safe main-thread point (no frame in
    // flight, sim not running).
    m_universeClient->flushDeferredWorldPackets();
    // Overlapped sim pipeline: instead of running the (~20-25ms) sim tick
    // here, defer it so render() can run it on the sim worker thread
    // CONCURRENTLY with world painting (pure GL from immutable renderData
    // snapshots). Everything below in this function then reads the previous
    // tick's state -- one fixed tick of extra latency for the camera/HUD
    // data, in exchange for the sim tick disappearing from the critical
    // path. Deferral only happens when the world render path is guaranteed
    // to run (in-world); otherwise the tick runs synchronously as before.
    if (worldClient && m_state > MainAppState::Title) {
      // The pending slot is guaranteed free here: any previously deferred
      // tick was flushed at the top of this function, before input handling.
      m_simTickDt = dt;
      m_simTickPending = true;
    } else {
      m_universeClient->update(dt);
    }
#else
    m_universeClient->update(dt);
#endif
    if (checkDisconnection())
      return;

    if (worldClient) {
      m_worldPainter->update(dt);
      auto& broadcastCallback = worldClient->broadcastCallback();
      if (!broadcastCallback) {
        broadcastCallback = [&](PlayerPtr player, StringView broadcast) -> bool {
          auto& view = broadcast.utf8();
          if (view.rfind(VoiceBroadcastPrefix.utf8(), 0) != NPos) {
            auto entityId = player->entityId();
            if (m_voice) {
              auto speaker = m_voice->speaker(connectionForEntity(entityId));
              speaker->entityId = entityId;
              speaker->name = player->name();
              speaker->position = player->mouthPosition();
              m_voice->receive(speaker, view.substr(VoiceBroadcastPrefix.utf8Size()));
            }
          }
          return true;
        };
      }

      if (worldClient->inWorld()) {
        if (needstoSendVoice) {
          auto signature = Curve25519::sign(voiceData.ptr(), voiceData.size());
          std::string_view signatureView((char*)signature.data(), signature.size());
          std::string_view audioDataView(voiceData.ptr(), voiceData.size());
          auto broadcast = strf("data\0voice\0{}{}"s, signatureView, audioDataView);
          worldClient->sendSecretBroadcast(broadcast, true, false); // Already compressed by Opus.
        }
        if (auto mainPlayer = m_universeClient->mainPlayer()) {
          if (m_voice) {
            auto localSpeaker = m_voice->localSpeaker();
            localSpeaker->position = mainPlayer->position();
            localSpeaker->entityId = mainPlayer->entityId();
            localSpeaker->name = mainPlayer->name();
          }
        }
        if (m_voice)
          m_voice->setLocalSpeaker(worldClient->connection());
      }
      worldClient->setInteractiveHighlightMode(isActionTaken(InterfaceAction::ShowLabels));
    }
#ifdef STAR_SYSTEM_SWITCH
    static int64_t s_urrTicks = 0, s_urrPre = 0, s_urrCamera = 0, s_urrIface = 0, s_urrMixer = 0;
    int64_t urrLapTime = Time::monotonicMicroseconds();
    auto urrLap = [&urrLapTime](int64_t& acc) {
      int64_t n = Time::monotonicMicroseconds();
      acc += n - urrLapTime;
      urrLapTime = n;
    };
    s_urrPre += urrLapTime; // marker reused below; replaced by lap semantics
    s_urrPre -= urrLapTime;
#endif
    updateCamera(dt);
#ifdef STAR_SYSTEM_SWITCH
    urrLap(s_urrCamera);
#endif

    m_cinematicOverlay->update(dt);
    m_mainInterface->update(dt);
#ifdef STAR_SYSTEM_SWITCH
    urrLap(s_urrIface);
#endif
    m_mainMixer->update(dt, m_cinematicOverlay->muteSfx(), m_cinematicOverlay->muteMusic());
    m_mainMixer->setSpeed(GlobalTimescale);
#ifdef STAR_SYSTEM_SWITCH
    urrLap(s_urrMixer);
    if (++s_urrTicks >= 150) {
      Logger::info("[perf-urr] camera={:.2f}ms iface={:.2f} mixer={:.2f}",
          s_urrCamera / 1e3 / s_urrTicks, s_urrIface / 1e3 / s_urrTicks, s_urrMixer / 1e3 / s_urrTicks);
      s_urrTicks = 0;
      s_urrCamera = s_urrIface = s_urrMixer = 0;
    }
#endif

    bool inputActive = m_mainInterface->textInputActive();
#ifdef STAR_SYSTEM_SWITCH
    runSwitchKeyboardSession(m_mainInterface->paneManager(), inputActive);
    inputActive = m_mainInterface->textInputActive();
#endif
    if (m_input)
      m_input->setTextInputActive(inputActive);
    if (inputActive)
      app->setTextArea(m_mainInterface->paneManager()->keyboardCapturedWidget()->keyboardCaptureArea());
    else
      app->setTextArea();
    app->setAcceptingTextInput(inputActive);

    for (auto const& interactAction : m_player->pullInteractActions())
      m_mainInterface->handleInteractAction(interactAction);

    if (m_universeServer) {
      if (auto p2pNetworkingService = app->p2pNetworkingService()) {
        for (auto& p2pClient : p2pNetworkingService->acceptP2PConnections())
          m_universeServer->addClient(UniverseConnection(P2PPacketSocket::open(std::move(p2pClient))));
      }

      m_universeServer->setPause(m_mainInterface->escapeDialogOpen());
    }

#if !STAR_SYSTEM_ANDROID && !STAR_SYSTEM_IOS
    Vec2F aimPosition = m_player->aimPosition();
    float fps = app->renderFps();
    LogMap::set("client_render_rate", strf("{:4.2f} FPS ({:4.2f}ms)", fps, (1.0f / app->renderFps()) * 1000.0f));
    LogMap::set("client_update_rate", strf("{:4.2f}Hz", app->updateRate()));
    LogMap::set("player_pos", strf("[ ^#f45;{:4.2f}^reset;, ^#49f;{:4.2f}^reset; ]", m_player->position()[0], m_player->position()[1]));
    LogMap::set("player_vel", strf("[ ^#f45;{:4.2f}^reset;, ^#49f;{:4.2f}^reset; ]", m_player->velocity()[0], m_player->velocity()[1]));
    LogMap::set("player_aim", strf("[ ^#f45;{:4.2f}^reset;, ^#49f;{:4.2f}^reset; ]", aimPosition[0], aimPosition[1]));
    if (auto world = m_universeClient->worldClient()) {
      auto aim = Vec2I::floor(aimPosition);
      LogMap::set("tile_liquid_level", toString(world->liquidLevel(aim).level));
      LogMap::set("tile_dungeon_id", world->isTileProtected(aim) ? strf("^red;{}", world->dungeonId(aim)) : toString(world->dungeonId(aim)));
    }
#endif

    if (m_mainInterface->currentState() == MainInterface::ReturnToTitle)
      changeState(MainAppState::Title);

  } catch (std::exception& e) {
    setError("Exception caught in client main-loop", e);
  }
}

bool ClientApplication::isActionTaken(InterfaceAction action) const {
  for (auto keyEvent : m_heldKeyEvents) {
    if (m_guiContext->actions(keyEvent).contains(action) || mobileDefaultActionMatch(keyEvent.key, action))
      return true;
  }

  return false;
}

bool ClientApplication::isActionTakenEdge(InterfaceAction action) const {
  for (auto keyEvent : m_edgeKeyEvents) {
    if (m_guiContext->actions(keyEvent).contains(action) || mobileDefaultActionMatch(keyEvent.key, action))
      return true;
  }

  return false;
}

#ifdef STAR_SYSTEM_SWITCH
void ClientApplication::runSwitchKeyboardSession(PaneManager* paneManager, bool inputActive) {
  // Text entry on Switch is one BLOCKING software-keyboard session per
  // textbox focus, driven here rather than through SDL's text-input
  // machinery. Two reasons: (a) the SDL switch port suppresses ALL touch
  // polling while SDL text input is active, and engine textboxes keep focus
  // (and therefore text input) until a touch blurs them -- a hard input
  // deadlock after the keyboard closes; (b) only the engine knows the
  // textbox's current content, so only this path can pre-fill the keyboard
  // and REPLACE the text on submit instead of appending to it.
  if (!inputActive) {
    m_switchKeyboardSessionDone = false;
    return;
  }
  if (m_switchKeyboardSessionDone)
    return;
  m_switchKeyboardSessionDone = true;

  auto widget = paneManager->keyboardCapturedWidget();
  auto textBox = as<TextBoxWidget>(widget);
  Logger::info("[swkbd] session begin ({})", textBox ? "textbox" : "non-textbox capture");
  if (textBox) {
    String entered;
    if (switchShowKeyboard(textBox->getText(), entered)) {
      textBox->setText(entered);
      // Keyboard OK doubles as Enter (console convention): commits the box
      // (chat send, dialog confirm) the same way a physical Return keypress
      // would. Route through processInput (not textBox->sendEvent directly):
      // a plain sendEvent only reaches the widget's own onEnterKeyCallback
      // (unset for chat), silently swallowing the event -- the actual
      // "commit" logic (ChatSendLine action -> doChat()) lives one layer up
      // in MainInterface::handleInputEvent, which only the full input
      // pipeline reaches. This previously made chat (and any other
      // onEnterKeyCallback-less textbox) discard the typed text and just
      // close on keyboard confirm.
      processInput(KeyDownEvent{Key::Return, KeyMod::NoMod});
      processInput(KeyUpEvent{Key::Return});
      Logger::info("[swkbd] session submit ({} chars)", entered.size());
    } else {
      Logger::info("[swkbd] session canceled");
    }
  }
  // Release keyboard capture either way: the session IS the edit. The widget
  // keeps its (new or old) content; focusing it again starts a new session.
  // Without this, a focused textbox would suppress game key bindings
  // indefinitely with no keyboard on screen.
  if (widget && widget->hasFocus())
    widget->blur();
}
#endif

void ClientApplication::updateCamera(float dt) {
  if (!m_universeClient->worldClient()) {
    m_cameraHistoryValid = false;
    return;
  }

  WorldCamera& camera = m_worldPainter->camera();
  camera.update(dt);

  if (m_mainInterface->fixedCamera()) {
    // Camera is being driven externally (cinematics, Lua): don't interpolate
    // against stale history.
    m_cameraHistoryValid = false;
    return;
  }

  auto assets = m_root->assets();

  const float triggerRadius = 100.0f;
  const float deadzone = 0.1f;
  const float panFactor = 1.5f;
  float cameraSpeedFactor = 30.0f / m_root->configuration()->get("cameraSpeedFactor").toFloat();
  cameraSpeedFactor /= (dt * 60.f);

  auto playerCameraPosition = m_player->cameraPosition();

  if (isActionTaken(InterfaceAction::CameraShift)) {
    m_snapBackCameraOffset = false;
    m_cameraOffsetDownTime += dt;
    Vec2F aim = m_universeClient->worldClient()->geometry().diff(m_mainInterface->cursorWorldPosition(), playerCameraPosition);

    float magnitude = aim.magnitude() / (triggerRadius / camera.pixelRatio());
    if (magnitude > deadzone) {
      float cameraXOffset = aim.x() / magnitude;
      float cameraYOffset = aim.y() / magnitude;
      magnitude = (magnitude - deadzone) / (1.0 - deadzone);
      if (magnitude > 1)
        magnitude = 1;
      cameraXOffset *= magnitude * 0.5f * camera.pixelRatio() * panFactor;
      cameraYOffset *= magnitude * 0.5f * camera.pixelRatio() * panFactor;
      m_cameraXOffset = (m_cameraXOffset * (cameraSpeedFactor - 1.0) + cameraXOffset) / cameraSpeedFactor;
      m_cameraYOffset = (m_cameraYOffset * (cameraSpeedFactor - 1.0) + cameraYOffset) / cameraSpeedFactor;
    }
  } else {
    if (m_cameraOffsetDownTime > 0.0f && m_cameraOffsetDownTime < 0.333333f)
      m_snapBackCameraOffset = true;
    if (m_snapBackCameraOffset) {
      m_cameraXOffset = (m_cameraXOffset * (cameraSpeedFactor - 1.0)) / cameraSpeedFactor;
      m_cameraYOffset = (m_cameraYOffset * (cameraSpeedFactor - 1.0)) / cameraSpeedFactor;
    }
    m_cameraOffsetDownTime = 0.f;
  }
  Vec2F newCameraPosition;

  newCameraPosition.setX(playerCameraPosition.x());
  newCameraPosition.setY(playerCameraPosition.y());

  auto baseCamera = newCameraPosition;

  const float cameraSmoothRadius = assets->json("/interface.config:cameraSmoothRadius").toFloat();
  const float cameraSmoothFactor = assets->json("/interface.config:cameraSmoothFactor").toFloat();

  auto cameraSmoothDistance = m_universeClient->worldClient()->geometry().diff(m_cameraPositionSmoother, newCameraPosition).magnitude();
  if (cameraSmoothDistance > cameraSmoothRadius) {
    auto cameraDelta = m_universeClient->worldClient()->geometry().diff(m_cameraPositionSmoother, newCameraPosition);
    m_cameraPositionSmoother = newCameraPosition + cameraDelta.normalized() * cameraSmoothRadius;
    m_cameraSmoothDelta = {};
  }

  auto cameraDelta = m_universeClient->worldClient()->geometry().diff(m_cameraPositionSmoother, newCameraPosition);
  if (cameraDelta.magnitude() > assets->json("/interface.config:cameraSmoothDeadzone").toFloat())
    newCameraPosition = newCameraPosition + cameraDelta * (cameraSmoothFactor - 1.0) / cameraSmoothFactor;
  m_cameraPositionSmoother = newCameraPosition;

  newCameraPosition.setX(newCameraPosition.x() + m_cameraXOffset / camera.pixelRatio());
  newCameraPosition.setY(newCameraPosition.y() + m_cameraYOffset / camera.pixelRatio());

  auto smoothDelta = newCameraPosition - baseCamera;

  m_worldPainter->setCameraPosition(m_universeClient->worldClient()->geometry(), baseCamera + (smoothDelta + m_cameraSmoothDelta) * 0.5f);
  m_cameraSmoothDelta = smoothDelta;

  // Record the last two tick positions (post-clamp, from the camera itself)
  // so render() can interpolate between them on sub-tick frames.
  m_cameraPrevPosition = m_cameraHistoryValid ? m_cameraCurPosition : camera.centerWorldPosition();
  m_cameraCurPosition = camera.centerWorldPosition();
  m_cameraHistoryValid = true;

  m_universeClient->worldClient()->setClientWindow(camera.worldTileRect());
}

}

STAR_MAIN_APPLICATION(Star::ClientApplication);
