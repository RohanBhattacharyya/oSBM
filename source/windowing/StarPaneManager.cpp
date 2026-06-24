#include "StarPaneManager.hpp"
#include "StarGameTypes.hpp"
#include "StarJsonExtra.hpp"
#include "StarAssets.hpp"
#include "StarRoot.hpp"
#include "StarImageWidget.hpp"
#include "StarLabelWidget.hpp"
#include "StarPortraitWidget.hpp"

#include <limits>

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
  if (auto navigation = event.ptr<UiNavigationEvent>())
    return handleUiNavigation(navigation->direction);

  if (m_hasUiSelection && !m_sendingUiNavigationMouseButton && (event.is<MouseButtonDownEvent>() || event.is<MouseButtonUpEvent>())) {
    if (event.is<MouseButtonDownEvent>())
      clearUiNavigationTextInputFocus();
    m_sendingUiNavigationMouseButton = true;
    bool handled = sendInputEvent(uiNavigationMouseEvent(event));
    m_sendingUiNavigationMouseButton = false;
    clearInvalidUiSelection();
    return handled;
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

void PaneManager::render() {
  if (m_backgroundWidget) {
    auto size = m_backgroundWidget->size();
    m_backgroundWidget->setPosition(Vec2I((windowSize()[0] - size[0]) / 2, (windowSize()[1] - size[1]) / 2));
    m_backgroundWidget->render(RectI(Vec2I(), windowSize()));
  }

  for (auto const& layerPair : reverseIterate(m_displayedPanes)) {
    for (auto const& panePair : reverseIterate(layerPair.second)) {
      if (panePair.first->active()) {
        if (m_prevInterfaceScale != m_context->interfaceScale())
          panePair.first->setPosition(
              calculateNewInterfacePosition(panePair.first, (float)m_context->interfaceScale() / m_prevInterfaceScale));

        panePair.first->setDrawingOffset(calculatePaneOffset(panePair.first));
        panePair.first->render(RectI(Vec2I(), windowSize()));
      }
    }
  }

  m_context->resetInterfaceScissorRect();
  drawUiSelection();
  m_prevInterfaceScale = m_context->interfaceScale();
}

void PaneManager::update(float dt) {
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

  for (auto const& layerPair : reverseIterate(m_displayedPanes)) {
    for (auto const& panePair : reverseIterate(layerPair.second)) {
      panePair.first->tick(dt);
      if (panePair.first->active())
        panePair.first->update(dt);
    }
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

  for (size_t i = 0; i < widget->numChildren(); ++i)
    collectUiNavigationCandidates(widget->getChildNum(i).get(), candidates, true, selectableAncestorRect);
}

bool PaneManager::uiNavigationCandidateWidget(Widget* widget) const {
  if (!widget->interactive() || widget->mouseTransparent())
    return false;
  if (is<LabelWidget>(widget) || is<ImageWidget>(widget) || is<PortraitWidget>(widget))
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

Vec2I PaneManager::uiNavigationSelectedCenter() const {
  return m_uiSelectionRect.center();
}

InputEvent PaneManager::uiNavigationMouseEvent(InputEvent const& event) const {
  Vec2F mousePosition = Vec2F(uiNavigationSelectedCenter()) * m_context->interfaceScale();
  if (auto down = event.ptr<MouseButtonDownEvent>())
    return MouseButtonDownEvent{down->mouseButton, mousePosition};
  if (auto up = event.ptr<MouseButtonUpEvent>())
    return MouseButtonUpEvent{up->mouseButton, mousePosition};
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
      panePair->first->dismissed();
      if (panePair->second)
        panePair->second(pane);
    }
  }

  return dismissed;
}

}
