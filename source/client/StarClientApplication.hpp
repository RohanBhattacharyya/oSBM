#pragma once

#include "StarUniverseServer.hpp"
#include "StarUniverseClient.hpp"
#include "StarWorldPainter.hpp"
#include "StarGameTypes.hpp"
#include "StarMainInterface.hpp"
#include "StarMainMixer.hpp"
#include "StarTitleScreen.hpp"
#include "StarErrorScreen.hpp"
#include "StarCinematic.hpp"
#include "StarKeyBindings.hpp"
#include "StarMainApplication.hpp"

namespace Star {

STAR_CLASS(Input);
STAR_CLASS(Voice);

class ClientApplication : public Application {
public:
  void setPostProcessLayerPasses(String const& layer, unsigned const& passes);
  void setPostProcessGroupEnabled(String const& group, bool const& enabled, Maybe<bool> const& save);
  bool postProcessGroupEnabled(String const& group);
  Json postProcessGroups();
  virtual unsigned framesSkipped() const override;

protected:
  virtual void startup(StringList const& cmdLineArgs) override;
  virtual void shutdown() override;

  virtual void applicationInit(ApplicationControllerPtr appController) override;
  virtual void renderInit(RendererPtr renderer) override;

  virtual void windowChanged(WindowMode windowMode, Vec2U screenSize) override;

  virtual void processInput(InputEvent const& event) override;

  virtual void update() override;
  virtual void render() override;

  virtual void getAudioData(int16_t* stream, size_t len) override;

private:
  enum class MainAppState {
    Quit,
    Startup,
    SteamFlatpakWarning,
    Mods,
    ModsWarning,
    Splash,
    Error,
    Title,
    SinglePlayer,
    MultiPlayer
  };

  struct PendingMultiPlayerConnection {
    Variant<P2PNetworkingPeerId, HostAddressWithPort> server;
    String account;
    String password;
    bool forceLegacy;
  };
  
  struct PostProcessGroup {
    bool enabled;
  };
  
  struct PostProcessLayer {
    List<String> effects;
    unsigned passes;
    PostProcessGroup* group;
  };

  void renderReload();

  void changeState(MainAppState newState);
  void setError(String const& error);
  void setError(String const& error, std::exception const& e);

  void loadMods();
  void updateSteamFlatpakWarning(float dt);
  void updateMods(float dt);
  void updateModsWarning(float dt);
  void updateSplash(float dt);
  void updateError(float dt);
  void updateTitle(float dt);
  void updateRunning(float dt);

  bool isActionTaken(InterfaceAction action) const;
  bool isActionTakenEdge(InterfaceAction action) const;

  void updateCamera(float dt);

#ifdef STAR_SYSTEM_SWITCH
  // One blocking software-keyboard session per textbox focus; see the
  // implementation for why this bypasses SDL text input entirely.
  void runSwitchKeyboardSession(PaneManager* paneManager, bool inputActive);
  bool m_switchKeyboardSessionDone = false;
#endif

  RootUPtr m_root;
  ThreadFunction<void> m_rootLoader;
  CallbackListenerPtr m_reloadListener;

  MainAppState m_state = MainAppState::Startup;

  // Valid after applicationInit is called
  MainMixerPtr m_mainMixer;
  GuiContextPtr m_guiContext;
  InputPtr m_input;
  VoicePtr m_voice;

  // Valid after renderInit is called the first time
  CinematicPtr m_cinematicOverlay;
  ErrorScreenPtr m_errorScreen;

  // Valid if main app state >= Title
  PlayerStoragePtr m_playerStorage;
  StatisticsPtr m_statistics;
  UniverseClientPtr m_universeClient;
  TitleScreenPtr m_titleScreen;

  // Valid if main app state > Title
  PlayerPtr m_player;
  WorldPainterPtr m_worldPainter;
  WorldRenderData m_renderData;
  MainInterfacePtr m_mainInterface;

#ifdef STAR_SYSTEM_SWITCH
  // Overlapped sim pipeline (see updateRunning/render): the deferred
  // universeClient tick runs on this persistent worker thread concurrently
  // with the GL-only world painting phase.
  void startSimTick();
  void joinSimTick();
  void stopSimThread();

  ThreadFunction<void> m_simThread;
  Mutex m_simMutex;
  ConditionVariable m_simCond;
  bool m_simRunRequested = false;
  bool m_simRunning = false;
  bool m_simShutdown = false;
  bool m_simTickPending = false;
  bool m_simTickInFlight = false;
  int64_t m_hudSyncCounter = 0;
  bool m_hudFbValid = false;
  bool m_autopilotActive = false;
  // Optional autopilot.flag contents: "stay" keeps the player on the ship
  // (no auto-beam), "warp=<action>" warps once to the parsed WarpAction
  // (e.g. warp=instanceworld:outpost) after arrival. Test infra only.
  bool m_autopilotStayOnShip = false;
  String m_autopilotWarpTarget;
  float m_pendingInterfaceDt = 0.0f;
  unsigned m_interfaceUpdateCounter = 0;
  float m_simTickDt = 0.0f;
  std::exception_ptr m_simException;
  int64_t m_simLastTickUs = 0; // written by worker, read after join
#endif
  
  StringMap<PostProcessGroup> m_postProcessGroups;
  List<PostProcessLayer> m_postProcessLayers;
  StringMap<size_t> m_labelledPostProcessLayers;

  // Valid if main app state == SinglePlayer
  UniverseServerPtr m_universeServer;

  float m_cameraXOffset = 0.0f;
  float m_cameraYOffset = 0.0f;
  bool m_snapBackCameraOffset = false;
  float m_cameraOffsetDownTime = 0.f;
  Vec2F m_cameraPositionSmoother;
  Vec2F m_cameraSmoothDelta;
  // Sub-tick render interpolation: camera positions from the last two sim
  // ticks (see updateCamera / render).
  Vec2F m_cameraPrevPosition;
  Vec2F m_cameraCurPosition;
  bool m_cameraHistoryValid = false;
  bool m_renderInterpolation = true;
  int m_cameraZoomDirection = 0;

  unsigned m_framesSkipped = 0;
  float m_minInterfaceScale = 2;
  float m_maxInterfaceScale = 3;
  Vec2F m_crossoverRes;

  bool m_controllerInput = false;
  Vec2F m_controllerLeftStick{};
  Vec2F m_controllerRightStick{};
  List<KeyDownEvent> m_heldKeyEvents;
  List<KeyDownEvent> m_edgeKeyEvents;

  Maybe<PendingMultiPlayerConnection> m_pendingMultiPlayerConnection;
  Maybe<HostAddressWithPort> m_currentRemoteJoin;
  int64_t m_timeSinceJoin = 0;

  ByteArray m_immediateFont;
  String m_windowTitle = "OpenStarbound";
  unsigned m_maxFrameSkipSetting = 5;
  float m_updateTrackWindowSetting = 1.0f;
  bool m_assetsBootstrapReady = false;

  bool m_loggedUGCCheck;
};

}
