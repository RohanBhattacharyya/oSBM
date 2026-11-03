#include "StarPaneManager.hpp"
#include "StarGameTypes.hpp"
#include "StarJsonExtra.hpp"
#include "StarAssets.hpp"
#include "StarRoot.hpp"
#include "StarLogging.hpp"
#include "StarTime.hpp"
#include "StarImageWidget.hpp"
#include "StarLabelWidget.hpp"
#include "StarPortraitWidget.hpp"
#include "StarItemGridWidget.hpp"
#include "StarItemSlotWidget.hpp"
#ifdef STAR_PLATFORM_MOBILE
#include "StarScrollArea.hpp"
#include "StarButtonWidget.hpp"
#include "StarTextBoxWidget.hpp"
#include "StarListWidget.hpp"
#include "StarSliderBar.hpp"
#endif

#include <limits>
#include <typeinfo>

namespace Star {

EnumMap<PaneLayer> const PaneLayerNames{
  {PaneLayer::Tooltip, "Tooltip"},
  {PaneLayer::ModalWindow, "ModalWindow"},
  {PaneLayer::Window, "Window"},
  {PaneLayer::Hud, "Hud"},
  {PaneLayer::World, "World"}
};

PaneManager::PaneManager()
  : m_context(GuiContext::singletonPtr()), m_prevInterfaceScale(1) {
  auto assets = Root::singleton().assets();
  m_tooltipMouseoverRadius = assets->json("/panes.config:tooltipMouseoverRadius").toFloat();
  m_tooltipMouseOffset = jsonToVec2I(assets->json("/panes.config:tooltipMouseoverOffset"));
  m_tooltipShowTimer = GameTimer(assets->json("/panes.config:tooltipMouseoverTime").toFloat());
}

void PaneManager::displayPane(PaneLayer paneLayer, PanePtr const& pane, DismissCallback onDismiss) {
  if (!m_displayedPanes[paneLayer].insertFront(pane, std::move(onDismiss)).second)
    throw GuiException("Pane displayed twice in PaneManager::displayPane");
  ++m_displayedPanesVersion;
#ifdef STAR_PLATFORM_MOBILE
  if (paneLayer != PaneLayer::Tooltip)
    ++m_touchWindowVersion;
#endif

  if (!pane->hasDisplayed() && pane->anchor() == PaneAnchor::None)
    pane->setPosition(Vec2I((windowSize() - pane->size()) / 2) + pane->centerOffset()); // center it

  pane->displayed();
}

bool PaneManager::isDisplayed(PanePtr const& pane) const {
  for (auto const& layerPair : m_displayedPanes) {
    if (layerPair.second.contains(pane))
      return true;
  }

  return false;
}

void PaneManager::dismissPane(PanePtr const& pane) {
  if (!dismiss(pane))
    throw GuiException("No such pane in PaneManager::dismissPane");
}

void PaneManager::dismissAllPanes(Set<PaneLayer> const& paneLayers) {
  for (auto const& paneLayer : paneLayers) {
    for (auto const& panePair : copy(m_displayedPanes[paneLayer]))
      dismiss(panePair.first);
  }
}

void PaneManager::dismissAllPanes() {
  for (auto layerPair : copy(m_displayedPanes)) {
    for (auto const& panePair : layerPair.second)
      dismiss(panePair.first);
  }
}

PanePtr PaneManager::topPane(Set<PaneLayer> const& paneLayers) const {
  for (auto const& layerPair : m_displayedPanes) {
    if (paneLayers.contains(layerPair.first) && !layerPair.second.empty())
      return layerPair.second.firstKey();
  }
  return {};
}

PanePtr PaneManager::topPane() const {
  for (auto const& layerPair : m_displayedPanes) {
    if (!layerPair.second.empty())
      return layerPair.second.firstKey();
  }
  return {};
}

void PaneManager::bringToTop(PanePtr const& pane) {
  for (auto& layerPair : m_displayedPanes) {
    if (layerPair.second.contains(pane)) {
      layerPair.second.toFront(pane);
      return;
    }
  }

  throw GuiException("Pane was not displayed in PaneManager::bringToTop");
}

void PaneManager::bringPaneAdjacent(PanePtr const& anchor, PanePtr const& adjacent, int gap) {
  Vec2I centerAdjacent = anchor->position() + (anchor->size() / 2) - (adjacent->size() / 2);
  centerAdjacent = centerAdjacent.piecewiseClamp(Vec2I(), windowSize() - adjacent->size()); // keeps pane inside window

  if (anchor->position()[0] + anchor->size()[0] + gap + adjacent->size()[0] <= windowSize()[0])
    adjacent->setPosition(Vec2I(anchor->position()[0] + anchor->size()[0] + gap, centerAdjacent[1])); // place to the right
  else if (anchor->position()[0] - gap - adjacent->size()[0] >= 0)
    adjacent->setPosition(Vec2I(anchor->position()[0] - gap - adjacent->size()[0], centerAdjacent[1])); // place to the left
  else if (anchor->position()[1] + anchor->size()[1] + gap + adjacent->size()[1] <= windowSize()[1])
    adjacent->setPosition(Vec2I(centerAdjacent[0], anchor->position()[1] + anchor->size()[1] + gap)); // place above
  else if (anchor->position()[1] - gap - adjacent->size()[1] >= 0)
    adjacent->setPosition(Vec2I(centerAdjacent[0], anchor->position()[1] - gap - adjacent->size()[1])); // place below
  else
    adjacent->setPosition(centerAdjacent);

  bringToTop(adjacent);
}

PanePtr PaneManager::getPaneAt(Set<PaneLayer> const& paneLayers, Vec2I const& position) const {
  for (auto const& layerPair : m_displayedPanes) {
    if (!paneLayers.contains(layerPair.first))
      continue;

    for (auto const& panePair : layerPair.second) {
      if (panePair.first->inWindow(position) && panePair.first->active())
        return panePair.first;
    }
  }

  return {};
}

PanePtr PaneManager::getPaneAt(Vec2I const& position) const {
  for (auto const& layerPair : m_displayedPanes) {
    for (auto const& panePair : layerPair.second) {
      if (panePair.first != m_activeTooltip
        && panePair.first->inWindow(position)
        && panePair.first->active())
        return panePair.first;
    }
  }

  return {};
}

List<PanePtr> PaneManager::getAllPanes() {
  List<PanePtr> list;
  for (auto const& layerPair : m_displayedPanes) {
    for (auto const& panePair : layerPair.second) {
      if (panePair.first != m_activeTooltip && panePair.first->active())
        list.append(panePair.first);
    }
  }
  return list;
}

void PaneManager::setBackgroundWidget(WidgetPtr bg) {
  m_backgroundWidget = bg;
}

void PaneManager::dismissWhere(function<bool(PanePtr const&)> func) {
  if (!func)
    return;

  for (auto& layerPair : m_displayedPanes) {
    eraseWhere(layerPair.second, [&](auto& panePair) {
      if (func(panePair.first)) {
        panePair.first->dismissed();
        if (panePair.second)
          panePair.second(panePair.first);
        return true;
      }
      return false;
    });
  }
}

PanePtr PaneManager::keyboardCapturedPane() const {
  for (auto const& layerPair : m_displayedPanes) {
    for (auto const& panePair : layerPair.second) {
      if (panePair.first->keyboardCapturer())
        return panePair.first;
    }
  }

  return {};
}

WidgetPtr PaneManager::keyboardCapturedWidget() const {
  for (auto const& layerPair : m_displayedPanes) {
    for (auto const& panePair : layerPair.second) {
      if (auto capturer = panePair.first->keyboardCapturer())
        return capturer;
    }
  }

  return {};
}

bool PaneManager::keyboardCapturedForTextInput() const {
  if (auto widget = keyboardCapturedWidget())
    return widget->keyboardCaptureMode() == KeyboardCaptureMode::TextInput;
  return false;
}

bool PaneManager::sendInputEvent(InputEvent const& event) {
#ifdef STAR_PLATFORM_MOBILE
  if (auto touch = event.ptr<TouchPointerEvent>())
    return handleTouchPointer(*touch);
  if (auto move = event.ptr<MouseMoveEvent>(); move && move->source == InputSource::Mouse && m_touchContact.active)
    cancelTouchPointer();
#endif
  if (m_hasUiSelection && !m_sendingUiNavigationMouseButton)
    clearInvalidUiSelection();
  if (auto navigation = event.ptr<UiNavigationEvent>())
    return handleUiNavigation(navigation->direction);

  if (m_hasUiSelection && !m_sendingUiNavigationMouseButton && (event.is<MouseButtonDownEvent>() || event.is<MouseButtonUpEvent>())) {
    if (event.is<MouseButtonDownEvent>())
      clearUiNavigationTextInputFocus();
    m_sendingUiNavigationMouseButton = true;
    sendInputEvent(uiNavigationMouseEvent(event));
    m_sendingUiNavigationMouseButton = false;
    clearInvalidUiSelection();
    // Even an inert focused widget must not fire a tool behind the window.
    return true;
  }

  if (event.is<MouseMoveEvent>()) {
    if (!m_sendingUiNavigationMouseMove) {
      m_hasUiSelection = false;
      m_uiSelectionWidget = nullptr;
    }

    m_tooltipLastMousePos = *m_context->mousePosition(event);

    for (auto const& layerPair : m_displayedPanes) {
      for (auto const& panePair : layerPair.second) {
        if (panePair.first->dragActive()) {
          panePair.first->drag(*m_context->mousePosition(event));
          return true;
        }
      }
    }
  }

  if (event.is<MouseButtonDownEvent>()) {
    m_tooltipShowTimer.reset();
    if (m_activeTooltip) {
      dismiss(m_activeTooltip);
      m_activeTooltip.reset();
      m_tooltipParentPane.reset();
      m_tooltipShowTimer.reset();
    }
  }

  if (event.is<MouseButtonUpEvent>()) {
    for (auto const& layerPair : m_displayedPanes) {
      for (auto const& panePair : layerPair.second) {
        if (panePair.first->dragActive()) {
          panePair.first->setDragActive(false, {});
          return true;
        }
      }
    }
  }

  // The gui close event can only be intercepted by a pane that has captured
  // the keyboard otherwise it will always be used to close first before being
  // a normal event. This is so a window can control its own closing if it
  // really needs to (like the keybindings window).
  if (event.is<KeyDownEvent>() && m_context->actions(event).contains(InterfaceAction::GuiClose)) {
    if (auto top = topPane({PaneLayer::ModalWindow, PaneLayer::Window})) {
      dismiss(top);
      return true;
    }
  }

  // If there is a pane that has captured the keyboard, keyboard events will
  // ONLY be sent to it.
  auto keyCapturePane = keyboardCapturedPane();
  if (keyCapturePane && (event.is<KeyDownEvent>() || event.is<KeyUpEvent>() || event.is<TextInputEvent>()))
    return keyCapturePane->sendEvent(event);

  bool foundModal = false;
  for (auto& layerPair : m_displayedPanes) {
    for (auto const& panePair : copy(layerPair.second)) {
      if (panePair.first->sendEvent(event)) {
        if (event.is<MouseButtonDownEvent>())
          layerPair.second.toFront(panePair.first);
        return true;
      }
      // If any modal windows are shown, Only the first modal window should
      // have a chance to consume the input event and all other panes below it
      // including different layers should ignore it.
      if (layerPair.first == PaneLayer::ModalWindow) {
        foundModal = true;
        break;
      }
    }
    if (foundModal)
      break;
  }

  return false;
}

#ifdef STAR_PLATFORM_MOBILE
static bool touchInventorySlot(WidgetPtr const& widget) {
  if (!is<ItemSlotWidget>(widget))
    return false;
  // Ícones de receitas dentro de listas devem rolar, sem pegar itens.
  for (Widget* parent = widget->parent(); parent; parent = parent->parent())
    if (is<ListWidget>(parent))
      return false;
  return true;
}

WidgetPtr PaneManager::pickTouchWidget(Vec2F position, float minimumSize, PanePtr& pane) {
  Vec2F guiPos = position / m_context->interfaceScale();
  float minimum = minimumSize / m_context->interfaceScale();
  WidgetPtr best;
  float score = std::numeric_limits<float>::max();
  bool modal = false;
  for (auto const& layer : m_displayedPanes) {
    if (layer.first == PaneLayer::Tooltip)
      continue;
    for (auto const& entry : layer.second) {
      auto const& candidatePane = entry.first;
      if (!candidatePane->active() || candidatePane->isDismissed())
        continue;
      auto visit = [&](auto&& self, WidgetPtr const& widget, RectI clip) -> void {
        if (!widget->active())
          return;
        RectI visible = widget->screenBoundRect().limited(clip);
        bool selectable = is<ButtonWidget>(widget) || is<ItemSlotWidget>(widget)
            || is<TextBoxWidget>(widget) || is<ScrollArea>(widget)
            || is<ListWidget>(widget) || is<SliderBarWidget>(widget);
        if (selectable && !visible.isEmpty() && widget->interactive() && !widget->mouseTransparent()) {
          Vec2F center(visible.center());
          Vec2F half = Vec2F(visible.size()).piecewiseMax(Vec2F::filled(minimum)) / 2;
          if (RectF::withCenter(center, half * 2).contains(guiPos)) {
            float distance = (center - guiPos).magnitudeSquared();
            // Área visual tem precedência sobre a expansão de outro botão.
            float value = distance + (visible.contains(Vec2I(guiPos)) ? 0 : 1000000);
            if (is<ScrollArea>(widget) || is<ListWidget>(widget))
              value += 100000;
            if (value < score) { score = value; best = widget; pane = candidatePane; }
          }
        }
        if (is<ItemSlotWidget>(widget) || is<ButtonWidget>(widget))
          return;
        if (is<ScrollArea>(widget))
          clip = clip.limited(widget->screenBoundRect());
        for (size_t i = 0; i < widget->numChildren(); ++i)
          self(self, widget->getChildNum(i), clip);
      };
      visit(visit, candidatePane, RectI::withSize({}, windowSize()));
      if (candidatePane->screenBoundRect().contains(Vec2I(guiPos)) || best)
        return best;
      if (layer.first == PaneLayer::ModalWindow) { modal = true; break; }
    }
    if (modal)
      break;
  }
  return best;
}

void PaneManager::touchClick(MouseButton button, Vec2F position, bool shifting) {
  bool oldShift = m_context->shiftHeld();
  sendInputEvent(MouseMoveEvent{{}, position, true, InputSource::Touch});
  if (shifting)
    m_context->setShiftHeld(true);
  sendInputEvent(MouseButtonDownEvent{button, position, InputSource::Touch});
  sendInputEvent(MouseButtonUpEvent{button, position, InputSource::Touch});
  m_context->setShiftHeld(oldShift);
}

void PaneManager::cancelTouchPointer() {
  if (m_touchSliderHeld) {
    sendInputEvent(MouseButtonUpEvent{MouseButton::Left, m_touchLast, InputSource::Touch});
  }
  m_touchSliderHeld = false;
  m_touchContact.clear();
  m_touchGesture.cancel();
  m_touchItemDrag = false;
  m_touchWidget.reset();
  m_touchScroll.reset();
  m_touchPane.reset();
  m_touchInertia[0] = m_touchInertia[1] = {};
  m_touchScrollFraction = {};
}

bool PaneManager::handleTouchPointer(TouchPointerEvent const& event) {
  if (m_touchPane && m_touchPanesVersion != m_touchWindowVersion)
    cancelTouchPointer();
  double now = Time::monotonicMilliseconds() / 1000.0;
  if (event.phase == TouchPhase::Cancel) {
    cancelTouchPointer();
    return true;
  }
  if (event.phase == TouchPhase::Down) {
    if (m_touchContact.active) {
      if (m_touchContact.owns(event.finger))
        return true;
      // O segundo contato só pode completar a transferência pendente.
      if (touchInventorySlot(m_touchWidget) && m_touchGesture.tap()
          && (event.position - m_touchLast).magnitude() < event.minimumTargetPixels * 1.5f) {
        touchClick(MouseButton::Left, m_touchStart, true);
        m_touchGesture.cancel();
      }
      return true;
    }
    cancelTouchPointer();
    m_touchContact.claim(event.finger);
    m_touchThreshold = std::max(2.0f, event.minimumTargetPixels / 4.8f);
    m_touchWidget = pickTouchWidget(event.position, event.minimumTargetPixels, m_touchPane);
    bool centerTarget = is<ButtonWidget>(m_touchWidget) || is<ItemSlotWidget>(m_touchWidget);
    m_touchStart = centerTarget ? Vec2F(m_touchWidget->screenBoundRect().center()) * m_context->interfaceScale() : event.position;
    m_touchLast = event.position;
    m_touchLastTime = now;
    m_touchGesture.down(event.position[0], event.position[1], now);
    m_touchPanesVersion = m_touchWindowVersion;
    if (m_touchWidget) {
      for (Widget* widget = m_touchWidget.get(); widget; widget = widget->parent()) {
        if (is<ScrollArea>(widget)) {
          m_touchScroll = widget == m_touchWidget.get() ? m_touchWidget : m_touchPane->childPtr(widget);
          break;
        }
      }
    }
    sendInputEvent(MouseMoveEvent{{}, m_touchStart, true, InputSource::Touch});
    return true;
  }
  if (!m_touchContact.owns(event.finger))
    return true;
  if (event.phase == TouchPhase::Move) {
    bool wasDragging = m_touchGesture.state == TouchInput::Gesture::State::Drag;
    bool dragging = m_touchGesture.move(event.position[0], event.position[1], m_touchThreshold);
    if (dragging && touchInventorySlot(m_touchWidget)) {
      if (!wasDragging) { touchClick(MouseButton::Left, m_touchStart); m_touchItemDrag = true; }
      sendInputEvent(MouseMoveEvent{{}, event.position, true, InputSource::Touch});
    } else if (dragging && is<SliderBarWidget>(m_touchWidget)) {
      if (!wasDragging) {
        sendInputEvent(MouseButtonDownEvent{MouseButton::Left,m_touchStart,InputSource::Touch});
        m_touchSliderHeld = true;
      }
      sendInputEvent(MouseMoveEvent{{},event.position,true,InputSource::Touch});
    } else if (dragging && m_touchScroll) {
      auto scroll = as<ScrollArea>(m_touchScroll);
      Vec2F delta = (event.position - m_touchLast) / m_context->interfaceScale();
      float seconds = std::max(0.001f, (float)(now - m_touchLastTime));
      m_touchInertia[0].velocity = std::clamp(-delta[0] / seconds, -1200.0f, 1200.0f);
      m_touchInertia[1].velocity = std::clamp(-delta[1] / seconds, -1200.0f, 1200.0f);
      m_touchScrollFraction -= delta;
      Vec2I amount = Vec2I::round(m_touchScrollFraction);
      m_touchScrollFraction -= Vec2F(amount);
      scroll->scrollAreaBy(amount);
    }
    m_touchLast = event.position;
    m_touchLastTime = now;
    return true;
  }
  if (event.phase == TouchPhase::Up) {
    if (is<SliderBarWidget>(m_touchWidget) && m_touchGesture.state == TouchInput::Gesture::State::Drag) {
      sendInputEvent(MouseButtonUpEvent{MouseButton::Left,event.position,InputSource::Touch});
      m_touchSliderHeld = false;
    } else if (m_touchItemDrag) {
      PanePtr pane;
      auto destination = pickTouchWidget(event.position, 0, pane);
      // Cruzar a borda da janela não descarta o item no mundo. Se não há
      // slot de destino, o item permanece no cursor do inventário.
      if (touchInventorySlot(destination))
        touchClick(MouseButton::Left, Vec2F(destination->screenBoundRect().center()) * m_context->interfaceScale());
    } else if (m_touchGesture.tap()) {
      if (m_touchWidget)
        touchClick(MouseButton::Left, m_touchStart);
      else if (!topPane({PaneLayer::ModalWindow})) {
        if (auto top = topPane({PaneLayer::Window}); top && !top->inWindow(Vec2I(event.position / m_context->interfaceScale())))
          dismissPane(top);
      }
    }
    m_touchContact.clear();
    m_touchGesture.cancel();
    m_touchItemDrag = false;
    m_touchWidget.reset();
    if (now - m_touchLastTime > 0.08)
      m_touchInertia[0] = m_touchInertia[1] = {};
    // Só a rolagem continua depois de levantar o dedo.
    if (!m_touchScroll)
      m_touchPane.reset();
    return true;
  }
  return true;
}

void PaneManager::updateTouchPointer(float dt) {
  if (m_touchPane && (!m_touchPane->isDisplayed() || m_touchPanesVersion != m_touchWindowVersion)) {
    cancelTouchPointer();
    return;
  }
  if (m_touchContact.active && is<ItemSlotWidget>(m_touchWidget)
      && m_touchGesture.longPress(Time::monotonicMilliseconds() / 1000.0))
    touchClick(MouseButton::Right, m_touchStart);
  if (!m_touchContact.active && m_touchScroll) {
    auto scroll = as<ScrollArea>(m_touchScroll);
    m_touchScrollFraction += Vec2F(m_touchInertia[0].step(dt), m_touchInertia[1].step(dt));
    Vec2I amount = Vec2I::round(m_touchScrollFraction);
    m_touchScrollFraction -= Vec2F(amount);
    scroll->scrollAreaBy(amount);
    if (m_touchInertia[0].velocity == 0 && m_touchInertia[1].velocity == 0) {
      m_touchScroll.reset(); m_touchPane.reset();
    }
  }
}
#endif

void PaneManager::setForceReplay(bool forceReplay) {
  m_forceReplay = forceReplay;
}

uint64_t PaneManager::displayedPanesVersion() const {
  return m_displayedPanesVersion;
}

bool PaneManager::allDisplayedReplayable() const {
  for (auto const& layerPair : m_displayedPanes) {
    for (auto const& panePair : layerPair.second) {
      if (panePair.first->active() && !m_paneRenderCache.contains(panePair.first.get()))
        return false;
    }
  }
  return true;
}

void PaneManager::render() {
  if (m_backgroundWidget) {
    auto size = m_backgroundWidget->size();
    m_backgroundWidget->setPosition(Vec2I((windowSize()[0] - size[0]) / 2, (windowSize()[1] - size[1]) / 2));
    m_backgroundWidget->render(RectI(Vec2I(), windowSize()));
  }

  // Self-measured frame pacing: only engage the pane replay cache when this
  // run is actually below ~20fps (same adaptive pattern as WorldClient's
  // entity render throttle). A machine with headroom renders every pane
  // fresh every frame, exactly as before.
  ++m_paneRenderFrame;
  int64_t nowUs = Time::monotonicMicroseconds();
  int64_t gapUs = m_lastRenderTimeUs != 0 ? nowUs - m_lastRenderTimeUs : 0;
  bool underLoad = gapUs > 50000;
  int64_t replayInterval = gapUs > 75000 ? 4 : 3;
  m_lastRenderTimeUs = nowUs;
  // Periodic sweep so recordings for dismissed panes don't accumulate. Never
  // on a force-replay frame: the caller was promised every displayed pane
  // can replay, and clearing here would force a fresh render mid-frame.
  if (m_paneRenderFrame % 300 == 0 && !m_forceReplay)
    m_paneRenderCache.clear();
  auto const& renderer = m_context->renderer();

#ifdef STAR_PLATFORM_MOBILE
  // Per-pane render cost ([perf-pm]): identifies which panes own the pane
  // layer's frame cost (fresh renders vs replays). Logged every ~150 frames.
  // All oSBM platforms (was Switch-only): the star-map/nav ScriptPane cost
  // investigation (issue #39) needs this in pulled iOS/Android/desktop logs.
  static int64_t s_pmFrames = 0, s_pmReplayUs = 0;
  static StringMap<int64_t> s_pmFreshUs;
#endif
  for (auto const& layerPair : reverseIterate(m_displayedPanes)) {
    for (auto const& panePair : reverseIterate(layerPair.second)) {
      if (panePair.first->active()) {
        if (m_prevInterfaceScale != m_context->interfaceScale())
          panePair.first->setPosition(
              calculateNewInterfacePosition(panePair.first, (float)m_context->interfaceScale() / m_prevInterfaceScale));

        panePair.first->setDrawingOffset(calculatePaneOffset(panePair.first));
        bool replayed = false;
#ifdef STAR_PLATFORM_MOBILE
        int64_t paneStart = Time::monotonicMicroseconds();
#endif
        if (underLoad || m_forceReplay) {
          Pane* paneKey = panePair.first.get();
          if (auto cached = m_paneRenderCache.ptr(paneKey)) {
            if (m_forceReplay || m_paneRenderFrame - cached->frame < replayInterval) {
              renderer->playPrimitiveRecording(cached->segments);
              replayed = true;
            }
          }
          if (!replayed) {
            bool recording = renderer->beginPrimitiveRecording();
            panePair.first->render(RectI(Vec2I(), windowSize()));
            if (recording)
              m_paneRenderCache[paneKey] = PaneRenderRecording{renderer->endPrimitiveRecording(), m_paneRenderFrame};
          }
        } else {
          panePair.first->render(RectI(Vec2I(), windowSize()));
        }
#ifdef STAR_PLATFORM_MOBILE
        int64_t paneUs = Time::monotonicMicroseconds() - paneStart;
        if (replayed)
          s_pmReplayUs += paneUs;
        else
          s_pmFreshUs[panePair.first->title().empty() ? String(typeid(*panePair.first).name()) : panePair.first->title()] += paneUs;
#endif
      }
    }
  }
#ifdef STAR_PLATFORM_MOBILE
  if (++s_pmFrames >= 150) {
    String breakdown;
    for (auto const& p : s_pmFreshUs)
      breakdown += strf(" {}={}us", p.first, p.second / s_pmFrames);
    Logger::info("[perf-pm] replay={:.1f}ms fresh:{}", s_pmReplayUs / 1e3 / s_pmFrames, breakdown);
    s_pmFrames = 0;
    s_pmReplayUs = 0;
    s_pmFreshUs.clear();
  }
#endif

  m_context->resetInterfaceScissorRect();
  drawUiSelection();
  m_prevInterfaceScale = m_context->interfaceScale();
}

void PaneManager::update(float dt) {
#ifdef STAR_PLATFORM_MOBILE
  updateTouchPointer(dt);
#endif
  auto newTooltipParentPane = getPaneAt(m_tooltipLastMousePos);

  bool updateTooltip = m_tooltipShowTimer.tick(dt) || (m_activeTooltip && (
    vmag(m_tooltipInitialPosition - m_tooltipLastMousePos) > m_tooltipMouseoverRadius
    || m_tooltipParentPane != newTooltipParentPane
    || !m_tooltipParentPane->inWindow(m_tooltipLastMousePos))); 

  if (updateTooltip) {
    if (m_activeTooltip) {
      dismiss(m_activeTooltip);
      m_activeTooltip.reset();
      m_tooltipParentPane.reset();
    }

    m_tooltipShowTimer.reset();
    if (newTooltipParentPane) {
      if (auto tooltip = newTooltipParentPane->createTooltip(m_tooltipLastMousePos)) {
        m_activeTooltip = std::move(tooltip);
        m_tooltipParentPane = std::move(newTooltipParentPane);
        m_tooltipInitialPosition = m_tooltipLastMousePos;
        displayPane(PaneLayer::Tooltip, m_activeTooltip);
      }
    }
  }

  if (m_activeTooltip) {
    Vec2I offsetDirection = Vec2I::filled(1);
    Vec2I offsetAdjust = Vec2I();

    if (m_tooltipLastMousePos[0] + m_tooltipMouseOffset[0] + m_activeTooltip->size()[0] > (int)m_context->windowWidth() / m_context->interfaceScale()) {
      offsetDirection[0] = -1;
      offsetAdjust[0] = -m_activeTooltip->size()[0];
    }

    if (m_tooltipLastMousePos[1] + m_tooltipMouseOffset[1] - m_activeTooltip->size()[1] < 0)
      offsetDirection[1] = -1;
    else
      offsetAdjust[1] = -m_activeTooltip->size()[1];

    m_activeTooltip->setPosition(m_tooltipLastMousePos + (offsetAdjust + m_tooltipMouseOffset.piecewiseMultiply(offsetDirection)));
  }

  for (auto const& layerPair : m_displayedPanes) {
    for (auto const& panePair : copy(layerPair.second)) {
      if (panePair.first->isDismissed())
        dismiss(panePair.first);
    }
  }

  // Under-load pane UPDATE throttle, companion to the render replay cache:
  // pane update() calls (widget-tree state refresh -- e.g. TeamBar rebuilds
  // its member list every call) cost ~4ms/frame. When this run is measurably
  // struggling (same 50ms heuristic as everywhere else) run them every 2nd
  // call, passing the ACCUMULATED dt so GameTimers and animations inside
  // panes stay wall-clock correct, just at half refresh granularity. tick()
  // (dismissal bookkeeping) stays per-frame -- it's cheap and dismissal
  // latency is user-visible.
  {
    int64_t nowUs = Time::monotonicMicroseconds();
    bool updateUnderLoad = m_lastUpdateTimeUs != 0 && (nowUs - m_lastUpdateTimeUs) > 50000;
    m_lastUpdateTimeUs = nowUs;
    m_pendingUpdateDt += dt;
    bool runUpdates = !updateUnderLoad || (++m_updateCounter % 2 == 0);
#ifdef STAR_PLATFORM_MOBILE
    // Per-pane update() cost ([perf-pmu]), the update-side companion of
    // [perf-pm]: for the nav/star-map ScriptPane the Lua update rebuilds the
    // whole canvas, so this line is where that cost shows up.
    static int64_t s_pmuFrames = 0;
    static StringMap<int64_t> s_pmuUs;
#endif
    for (auto const& layerPair : reverseIterate(m_displayedPanes)) {
      for (auto const& panePair : reverseIterate(layerPair.second)) {
        panePair.first->tick(dt);
        if (runUpdates && panePair.first->active()) {
#ifdef STAR_PLATFORM_MOBILE
          int64_t updStart = Time::monotonicMicroseconds();
          panePair.first->update(m_pendingUpdateDt);
          s_pmuUs[panePair.first->title().empty() ? String(typeid(*panePair.first).name()) : panePair.first->title()] +=
              Time::monotonicMicroseconds() - updStart;
#else
          panePair.first->update(m_pendingUpdateDt);
#endif
        }
      }
    }
    if (runUpdates)
      m_pendingUpdateDt = 0.0f;
#ifdef STAR_PLATFORM_MOBILE
    if (++s_pmuFrames >= 150) {
      if (!s_pmuUs.empty()) {
        String breakdown;
        for (auto const& p : s_pmuUs)
          breakdown += strf(" {}={}us", p.first, p.second / s_pmuFrames);
        Logger::info("[perf-pmu] update:{}", breakdown);
      }
      s_pmuFrames = 0;
      s_pmuUs.clear();
    }
#endif
  }

  clearInvalidUiSelection();
}

bool PaneManager::handleUiNavigation(UiNavigationDirection direction) {
  clearUiNavigationTextInputFocus();

  List<UiNavigationCandidate> candidates;
  bool foundModal = false;
  for (auto const& layerPair : m_displayedPanes) {
    for (auto const& panePair : layerPair.second) {
      if (panePair.first == m_activeTooltip || !panePair.first->active())
        continue;

      collectUiNavigationCandidates(panePair.first.get(), candidates, false);

      if (layerPair.first == PaneLayer::ModalWindow) {
        foundModal = true;
        break;
      }
    }
    if (foundModal)
      break;
  }

  auto selected = chooseUiNavigationCandidate(candidates, direction);
  if (!selected)
    return false;

  m_hasUiSelection = true;
  m_uiSelectionWidget = selected->widget;
  m_uiSelectionRect = selected->rect;

  Vec2F mousePosition = Vec2F(selected->center) * m_context->interfaceScale();
  MouseMoveEvent moveEvent{{0.0f, 0.0f}, mousePosition, true};
  m_sendingUiNavigationMouseMove = true;
  sendInputEvent(moveEvent);
  m_sendingUiNavigationMouseMove = false;
  return true;
}

void PaneManager::collectUiNavigationCandidates(Widget* widget, List<UiNavigationCandidate>& candidates, bool includeSelf, Maybe<RectI> selectableAncestorRect) const {
  if (!widget || !widget->active())
    return;

  bool selectableSelf = includeSelf && uiNavigationCandidateWidget(widget);

  if (selectableSelf) {
    RectI rect = widget->screenBoundRect();
    if (!rect.isEmpty() && rect.size()[0] >= 3 && rect.size()[1] >= 3) {
      RectI visibleRect = rect.limited(RectI::withSize(Vec2I(), windowSize()));
      if (!visibleRect.isEmpty()) {
        candidates.append({widget, visibleRect, visibleRect.center(), selectableAncestorRect});
        selectableAncestorRect = visibleRect;
      }
    }
  }

  // Slot decorations (durability/count overlays) are not separate targets.
  if (is<ItemSlotWidget>(widget))
    return;

  for (size_t i = 0; i < widget->numChildren(); ++i)
    collectUiNavigationCandidates(widget->getChildNum(i).get(), candidates, true, selectableAncestorRect);
}

bool PaneManager::uiNavigationCandidateWidget(Widget* widget) const {
  if (!widget->interactive() || widget->mouseTransparent())
    return false;
  // Select individual item slots, never the bounding box of the entire bag.
  if (is<ItemGridWidget>(widget) || is<LabelWidget>(widget) || is<ImageWidget>(widget) || is<PortraitWidget>(widget))
    return false;
  return true;
}

Maybe<PaneManager::UiNavigationCandidate> PaneManager::chooseUiNavigationCandidate(List<UiNavigationCandidate> const& candidates, UiNavigationDirection direction) const {
  if (candidates.empty())
    return {};

  Vec2I origin = m_hasUiSelection ? m_uiSelectionRect.center() : uiNavigationOrigin(direction);
  Maybe<UiNavigationCandidate> best;
  float bestScore = std::numeric_limits<float>::max();

  for (auto const& candidate : candidates) {
    Vec2I delta = candidate.center - origin;
    float primary;
    float perpendicular;
    switch (direction) {
      case UiNavigationDirection::Up:
        primary = delta[1];
        perpendicular = std::abs(delta[0]);
        break;
      case UiNavigationDirection::Down:
        primary = -delta[1];
        perpendicular = std::abs(delta[0]);
        break;
      case UiNavigationDirection::Left:
        primary = -delta[0];
        perpendicular = std::abs(delta[1]);
        break;
      case UiNavigationDirection::Right:
      default:
        primary = delta[0];
        perpendicular = std::abs(delta[1]);
        break;
    }

    if (primary <= 0.0f)
      continue;

    float score = primary + perpendicular * 2.0f;
    if (candidate.selectableAncestorRect && (!m_hasUiSelection || *candidate.selectableAncestorRect != m_uiSelectionRect))
      score += 100000.0f;

    if (score < bestScore) {
      bestScore = score;
      best = candidate;
    }
  }

  return best;
}

Vec2I PaneManager::uiNavigationOrigin(UiNavigationDirection direction) const {
  Vec2I size = windowSize();
  switch (direction) {
    case UiNavigationDirection::Up:
      return {size[0] / 2, 0};
    case UiNavigationDirection::Down:
      return {size[0] / 2, size[1]};
    case UiNavigationDirection::Left:
      return {size[0], size[1] / 2};
    case UiNavigationDirection::Right:
    default:
      return {0, size[1] / 2};
  }
}

Maybe<Vec2F> PaneManager::uiNavigationPosition() const {
  if (m_hasUiSelection)
    return Vec2F(uiNavigationSelectedCenter()) * m_context->interfaceScale();
  return {};
}

Vec2I PaneManager::uiNavigationSelectedCenter() const {
  return m_uiSelectionRect.center();
}

InputEvent PaneManager::uiNavigationMouseEvent(InputEvent const& event) const {
  Vec2F mousePosition = Vec2F(uiNavigationSelectedCenter()) * m_context->interfaceScale();
  if (auto down = event.ptr<MouseButtonDownEvent>())
    return MouseButtonDownEvent{down->mouseButton, mousePosition, down->source};
  if (auto up = event.ptr<MouseButtonUpEvent>())
    return MouseButtonUpEvent{up->mouseButton, mousePosition, up->source};
  return event;
}

void PaneManager::clearUiNavigationTextInputFocus() {
  if (auto widget = keyboardCapturedWidget()) {
    if (widget->keyboardCaptureMode() == KeyboardCaptureMode::TextInput)
      widget->blur();
  }
}

void PaneManager::clearInvalidUiSelection() {
  if (!m_hasUiSelection)
    return;

  List<UiNavigationCandidate> candidates;
  bool foundModal = false;
  for (auto const& layerPair : m_displayedPanes) {
    for (auto const& panePair : layerPair.second) {
      if (panePair.first == m_activeTooltip || panePair.first->isDismissed() || !panePair.first->active())
        continue;

      collectUiNavigationCandidates(panePair.first.get(), candidates, false);

      if (layerPair.first == PaneLayer::ModalWindow) {
        foundModal = true;
        break;
      }
    }
    if (foundModal)
      break;
  }

  for (auto const& candidate : candidates) {
    if (candidate.widget == m_uiSelectionWidget) {
      m_uiSelectionRect = candidate.rect;
      return;
    }
  }

  m_hasUiSelection = false;
  m_uiSelectionWidget = nullptr;
}

void PaneManager::drawUiSelection() const {
  if (!m_hasUiSelection)
    return;

  RectI rect = m_uiSelectionRect.padded(3);
  m_context->drawInterfaceQuad(RectF(rect), Vec4B(80, 180, 255, 36));
  m_context->drawInterfacePolyLines(PolyF(rect), Vec4B(120, 220, 255, 230), 2.0f);
}

Vec2I PaneManager::windowSize() const {
  return Vec2I(m_context->windowInterfaceSize());
}

Vec2I PaneManager::calculatePaneOffset(PanePtr const& pane) const {
  Vec2I size = pane->size();
  switch (pane->anchor()) {
    case PaneAnchor::None:
      return pane->anchorOffset();
    case PaneAnchor::BottomLeft:
      return pane->anchorOffset();
    case PaneAnchor::BottomRight:
      return pane->anchorOffset() + Vec2I{windowSize()[0] - size[0], 0};
    case PaneAnchor::TopLeft:
      return pane->anchorOffset() + Vec2I{0, windowSize()[1] - size[1]};
    case PaneAnchor::TopRight:
      return pane->anchorOffset() + (windowSize() - size);
    case PaneAnchor::CenterTop:
      return pane->anchorOffset() + Vec2I{(windowSize()[0] - size[0]) / 2, windowSize()[1] - size[1]};
    case PaneAnchor::CenterBottom:
      return pane->anchorOffset() + Vec2I{(windowSize()[0] - size[0]) / 2, 0};
    case PaneAnchor::CenterLeft:
      return pane->anchorOffset() + Vec2I{0, (windowSize()[1] - size[1]) / 2};
    case PaneAnchor::CenterRight:
      return pane->anchorOffset() + Vec2I{windowSize()[0] - size[0], (windowSize()[1] - size[1]) / 2};
    case PaneAnchor::Center:
      return pane->anchorOffset() + ((windowSize() - size) / 2);
    default:
      return pane->anchorOffset();
  }
}

Vec2I PaneManager::calculateNewInterfacePosition(PanePtr const& pane, float interfaceScaleRatio) const {
  Vec2F position(pane->relativePosition());
  Vec2F size(pane->size());
  Mat3F scale;
  switch (pane->anchor()) {
    case PaneAnchor::None:
      scale = Mat3F::scaling(interfaceScaleRatio, Vec2F(windowSize()) / 2);
      break;
    case PaneAnchor::BottomLeft:
      scale = Mat3F::scaling(interfaceScaleRatio);
      break;
    case PaneAnchor::BottomRight:
      scale = Mat3F::scaling(interfaceScaleRatio, {size[0], 0});
      break;
    case PaneAnchor::TopLeft:
      scale = Mat3F::scaling(interfaceScaleRatio, {0, size[1]});
      break;
    case PaneAnchor::TopRight:
      scale = Mat3F::scaling(interfaceScaleRatio, size);
      break;
    case PaneAnchor::CenterTop:
      scale = Mat3F::scaling(interfaceScaleRatio, {size[0] / 2, size[1]});
      break;
    case PaneAnchor::CenterBottom:
      scale = Mat3F::scaling(interfaceScaleRatio, {size[0] / 2, 0});
      break;
    case PaneAnchor::CenterLeft:
      scale = Mat3F::scaling(interfaceScaleRatio, {0, size[1] / 2});
      break;
    case PaneAnchor::CenterRight:
      scale = Mat3F::scaling(interfaceScaleRatio, {size[0], size[1] / 2});
      break;
    case PaneAnchor::Center:
      scale = Mat3F::scaling(interfaceScaleRatio, size / 2);
      break;
    default:
      scale = Mat3F::scaling(interfaceScaleRatio, Vec2F(windowSize()) / 2);
  }
  return Vec2I::round((scale * Vec3F(position, 0)).vec2());
}

bool PaneManager::dismiss(PanePtr const& pane) {
  bool dismissed = false;
  for (auto& layerPair : m_displayedPanes) {
    if (auto panePair = layerPair.second.maybeTake(pane)) {
      dismissed = true;
      ++m_displayedPanesVersion;
#ifdef STAR_PLATFORM_MOBILE
      if (layerPair.first != PaneLayer::Tooltip)
        ++m_touchWindowVersion;
#endif
      panePair->first->dismissed();
      if (panePair->second)
        panePair->second(pane);
    }
  }

  return dismissed;
}

}
