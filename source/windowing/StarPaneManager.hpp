#pragma once

#include "StarPane.hpp"
#include "StarOrderedMap.hpp"
#include "StarBiMap.hpp"
#include "StarGameTimers.hpp"
#include "StarRenderer.hpp"

namespace Star {

STAR_CLASS(PaneManager);

enum class PaneLayer {
  // A special class of window only meant to be used by PaneManager to display
  // tooltips given by Pane::createTooltip
  Tooltip,
  // A special class of window that is displayed above all other windows and
  // turns off input to other windows and the hud until it is dismissed.
  ModalWindow,
  // Window layer for regular windows that are regularly displayed and
  // dismissed and dragged around.
  Window,
  // The bottom GUI layer, for persistent hud elements that are always or almost
  // always shown.  Not key dismissable.
  Hud,
  // Layer for interface elements which are logically part of the world but
  // handled by GUI panes (such as wires)
  World
};
extern EnumMap<PaneLayer> const PaneLayerNames;


// This class handles a set of panes to be drawn as a collective windowing
// interface.  It is a set of panes on separate distinct layers, where each
// layer contains a z-ordered list of panes to display.
class PaneManager {
public:
  typedef function<void(PanePtr const&)> DismissCallback;

  PaneManager();

  // Display a pane on any given layer.  The pane lifetime in this class is
  // only during display, once dismissed, the pane is forgotten completely.
  void displayPane(PaneLayer paneLayer, PanePtr const& pane, DismissCallback onDismiss = {});

  bool isDisplayed(PanePtr const& pane) const;

  // Dismiss a given displayed pane.  Pane must already be displayed.
  void dismissPane(PanePtr const& pane);

  // Dismisses all panes in the given layers.
  void dismissAllPanes(Set<PaneLayer> const& paneLayers);
  void dismissAllPanes();

  PanePtr topPane(Set<PaneLayer> const& paneLayers) const;
  PanePtr topPane() const;

  // Brign an already displayed pane to the top of its layer.
  void bringToTop(PanePtr const& pane);

  // Position a pane adjacent to an anchor pane in a direction where
  // it will fit on the screen
  void bringPaneAdjacent(PanePtr const& anchor, PanePtr const& adjacent, int gap);

  PanePtr getPaneAt(Set<PaneLayer> const& paneLayers, Vec2I const& position) const;
  PanePtr getPaneAt(Vec2I const& position) const;
  List<PanePtr> getAllPanes();

  void setBackgroundWidget(WidgetPtr bg);

  void dismissWhere(function<bool(PanePtr const&)> func);

  // Returns the pane/widget that has captured the keyboard, if any.
  PanePtr keyboardCapturedPane() const;
  WidgetPtr keyboardCapturedWidget() const;
  // Returns true if the current widget that has captured the keyboard is
  // accepting text input.
  bool keyboardCapturedForTextInput() const;

  bool sendInputEvent(InputEvent const& event);

  void render();
  void update(float dt);

private:
  struct UiNavigationCandidate {
    Widget const* widget;
    RectI rect;
    Vec2I center;
    Maybe<RectI> selectableAncestorRect;
  };

  Vec2I windowSize() const;
  Vec2I calculatePaneOffset(PanePtr const& pane) const;
  Vec2I calculateNewInterfacePosition(PanePtr const& pane, float interfaceScaleRatio) const;
  bool dismiss(PanePtr const& pane);
  bool handleUiNavigation(UiNavigationDirection direction);
  void collectUiNavigationCandidates(Widget* widget, List<UiNavigationCandidate>& candidates, bool includeSelf, Maybe<RectI> selectableAncestorRect = {}) const;
  bool uiNavigationCandidateWidget(Widget* widget) const;
  Maybe<UiNavigationCandidate> chooseUiNavigationCandidate(List<UiNavigationCandidate> const& candidates, UiNavigationDirection direction) const;
  Vec2I uiNavigationOrigin(UiNavigationDirection direction) const;
  Vec2I uiNavigationSelectedCenter() const;
  InputEvent uiNavigationMouseEvent(InputEvent const& event) const;
  void clearUiNavigationTextInputFocus();
  void clearInvalidUiSelection();
  void drawUiSelection() const;

  GuiContext* m_context;
  float m_prevInterfaceScale;

  // Under-load pane render replay cache: pane rendering is immediate-mode (a
  // full widget-tree walk emitting primitives every frame) and costs
  // ~15-20ms/frame of HUD on weak hardware even when nothing on screen
  // changed. When this instance is measurably struggling (see underLoad in
  // render()), each pane is freshly rendered only every 3rd frame and its
  // recorded primitives (with scissor state) are replayed in between --
  // identical output, just up to 2 frames stale, and only ever engaged below
  // ~20fps where UI staleness of ~100-150ms is imperceptible next to the
  // frame stutter itself.
  struct PaneRenderRecording {
    List<Renderer::RecordedSegment> segments;
    int64_t frame = 0;
  };
  HashMap<Pane*, PaneRenderRecording> m_paneRenderCache;
  int64_t m_paneRenderFrame = 0;
  int64_t m_lastRenderTimeUs = 0;

  // Under-load pane update() throttle state (see update()).
  int64_t m_lastUpdateTimeUs = 0;
  int64_t m_updateCounter = 0;
  float m_pendingUpdateDt = 0.0f;

  // Map of each pane layer, where the 0th pane is the topmost pane in each layer.
  Map<PaneLayer, OrderedMap<PanePtr, DismissCallback>> m_displayedPanes;

  WidgetPtr m_backgroundWidget;

  float m_tooltipMouseoverRadius;
  Vec2I m_tooltipMouseOffset;
  GameTimer m_tooltipShowTimer;
  Vec2I m_tooltipLastMousePos;
  Vec2I m_tooltipInitialPosition;
  PanePtr m_activeTooltip;
  PanePtr m_tooltipParentPane;

  bool m_hasUiSelection = false;
  bool m_sendingUiNavigationMouseMove = false;
  bool m_sendingUiNavigationMouseButton = false;
  Widget const* m_uiSelectionWidget = nullptr;
  RectI m_uiSelectionRect;
};

}
