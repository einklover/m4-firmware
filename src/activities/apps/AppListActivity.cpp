#include "AppListActivity.h"

#include <GfxRenderer.h>

#include "AppInstallActivity.h"
#include "AppRuntimeActivity.h"
#include "MappedInputManager.h"
#include "apps/M4xInstaller.h"
#include "apps/M4xPaths.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/M4ListTouchPolicy.h"

#include <algorithm>

namespace {
constexpr unsigned long kAppLongPressMs = 700;

M4ListTouchPolicy::DialogTwoButtonLayout uninstallDialogLayout(const GfxRenderer& renderer) {
  return M4ListTouchPolicy::makeCenteredTwoButtons(renderer.getScreenWidth(), renderer.getScreenHeight() - 190,
                                                   144, 64, 24, 2);
}

TouchHitGeometry::Rect uninstallDataToggleRect(const GfxRenderer& renderer) {
  constexpr int width = 260;
  constexpr int height = 52;
  return {std::max(0, (renderer.getScreenWidth() - width) / 2), renderer.getScreenHeight() - 290, width, height};
}
}

AppListActivity::AppListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void()>& onGoBack)
    : ActivityWithSubactivity("AppList", renderer, mappedInput), onGoBack(onGoBack) {}

void AppListActivity::taskTrampoline(void* param) {
  auto* self = static_cast<AppListActivity*>(param);
  self->displayTaskLoop();
}

void AppListActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired_ && !subActivity) {
      updateRequired_ = false;
      xSemaphoreTake(renderingMutex_, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex_);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void AppListActivity::reload() {
  M4xInstaller::ensureLayout();
  apps_ = M4xRegistry::load();
  if (selectedIndex_ >= static_cast<int>(apps_.size())) {
    selectedIndex_ = std::max(0, static_cast<int>(apps_.size()) - 1);
  }
  mode_ = 0;
}

void AppListActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  renderingMutex_ = xSemaphoreCreateMutex();
  reload();
  updateRequired_ = true;
  xTaskCreate(&AppListActivity::taskTrampoline, "AppList", 4096, this, 1, &displayTaskHandle_);
}

void AppListActivity::onExit() {
  ActivityWithSubactivity::onExit();
  xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  if (displayTaskHandle_) {
    vTaskDelete(displayTaskHandle_);
    displayTaskHandle_ = nullptr;
  }
  vSemaphoreDelete(renderingMutex_);
  renderingMutex_ = nullptr;
}

void AppListActivity::openSelected() {
  if (apps_.empty() || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(apps_.size())) return;
  const auto app = apps_[static_cast<size_t>(selectedIndex_)];
  xSemaphoreTake(renderingMutex_, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new AppRuntimeActivity(
      renderer, mappedInput, app, [this]() {
        exitActivity();
        reload();
        updateRequired_ = true;
      }));
  xSemaphoreGive(renderingMutex_);
}

void AppListActivity::uninstallSelected() {
  if (apps_.empty() || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(apps_.size())) return;
  std::string err;
  if (!M4xInstaller::uninstall(apps_[static_cast<size_t>(selectedIndex_)].id, uninstallClearData_, err)) {
    Serial.printf("[M4x] uninstall failed: %s\n", err.c_str());
  }
  reload();
  updateRequired_ = true;
}

void AppListActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    if (mode_ == 1) {
      mode_ = 0;
      updateRequired_ = true;
      return;
    }
    onGoBack();
    return;
  }

  const int count = static_cast<int>(apps_.size());

  if (mode_ == 1) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const auto dialog = uninstallDialogLayout(renderer);
      int hit = -1;
      if (M4ListTouchPolicy::dialogButtonFromPoint(dialog, tx, ty, hit)) {
        if (hit == 0) {
          mode_ = 0;
          updateRequired_ = true;
        } else {
          uninstallSelected();
        }
      } else if (uninstallDataToggleRect(renderer).contains(tx, ty)) {
        uninstallClearData_ = !uninstallClearData_;
        updateRequired_ = true;
      } else {
        // Ignore taps outside an explicit action target.
        return;
      }
      /*
       * Confirmation screen deliberately does not use whole-screen halves:
       * the touch targets are visible and match the hit-test rectangles.
       */
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      uninstallSelected();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      mode_ = 0;
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      uninstallClearData_ = !uninstallClearData_;
      updateRequired_ = true;
    }
    return;
  }

  // Touch list + install inbox shortcut
  if (mappedInput.hasTouch() && count > 0) {
    auto metrics = UITheme::getInstance().getMetrics();
    const int pageHeight = renderer.getScreenHeight();
    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    int footerX = 0, footerY = 0;
    const bool tapped = mappedInput.wasScreenTapped(footerX, footerY);
    if (tapped && footerY >= pageHeight - metrics.buttonHintsHeight) {
      const int quarter = renderer.getScreenWidth() / 4;
      if (footerX < quarter) {
        onGoBack();
      } else if (footerX < quarter * 2) {
        openSelected();
      } else if (footerX < quarter * 3) {
        mode_ = 1;
        updateRequired_ = true;
      } else {
        xSemaphoreTake(renderingMutex_, portMAX_DELAY);
        exitActivity();
        enterNewActivity(new AppInstallActivity(renderer, mappedInput, "", [this]() {
          exitActivity();
          reload();
          updateRequired_ = true;
        }));
        xSemaphoreGive(renderingMutex_);
      }
      return;
    }
    // A completed long touch is reported as a tap too. Route it to the
    // explicit uninstall confirmation before normal row activation.
    if (tapped && mappedInput.lastScreenTouchHeldMs() >= kAppLongPressMs) {
      int longPressedIndex = -1;
      if (TouchHitGeometry::listIndexFromPoint(footerY, listTop, listHeight, metrics.listRowHeight, count,
                                               selectedIndex_, longPressedIndex)) {
        selectedIndex_ = longPressedIndex;
        mode_ = 1;
        updateRequired_ = true;
        return;
      }
    }
    M4ListTouchPolicy::Event te{};
    const auto sw = mappedInput.wasSwipe();
    if (sw == MappedInputManager::SwipeDir::Up) te.swipe = M4ListTouchPolicy::Swipe::Up;
    else if (sw == MappedInputManager::SwipeDir::Down) te.swipe = M4ListTouchPolicy::Swipe::Down;
    int dx = 0, dy = 0;
    te = M4ListTouchPolicy::mergeFrame(false, te.swipe, mappedInput.wasScreenTouchDown(dx, dy), dx, dy,
                                       tapped, footerX, footerY);
    M4ListTouchPolicy::ListLayout layout;
    layout.listTop = listTop;
    layout.listHeight = listHeight;
    layout.rowStep = metrics.listRowHeight;
    layout.itemCount = count;
    layout.selectedIndex = selectedIndex_;
    const int pageItems = std::max(1, listHeight / metrics.listRowHeight);
    int hit = -1;
    const auto act = M4ListTouchPolicy::resolveList(te, layout, hit);
    if (act == M4ListTouchPolicy::Action::PageDown || act == M4ListTouchPolicy::Action::PageUp) {
      selectedIndex_ = M4ListTouchPolicy::applyPage(selectedIndex_, count, pageItems,
                                                    act == M4ListTouchPolicy::Action::PageDown);
      updateRequired_ = true;
      return;
    }
    if (act == M4ListTouchPolicy::Action::Select && hit >= 0) {
      selectedIndex_ = hit;
      updateRequired_ = true;
      return;
    }
    if (act == M4ListTouchPolicy::Action::Activate && hit >= 0) {
      selectedIndex_ = hit;
      openSelected();
      return;
    }
  } else if (mappedInput.hasTouch()) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      // Empty list: open installer from inbox.
      xSemaphoreTake(renderingMutex_, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new AppInstallActivity(renderer, mappedInput, "", [this]() {
        exitActivity();
        reload();
        updateRequired_ = true;
      }));
      xSemaphoreGive(renderingMutex_);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (count > 0) openSelected();
    else {
      xSemaphoreTake(renderingMutex_, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new AppInstallActivity(renderer, mappedInput, "", [this]() {
        exitActivity();
        reload();
        updateRequired_ = true;
      }));
      xSemaphoreGive(renderingMutex_);
    }
    return;
  }

  // Long-press confirm = uninstall menu
  if (mappedInput.getHeldTime() > 700 && mappedInput.wasReleased(MappedInputManager::Button::Confirm) == false &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && count > 0) {
    // skip — use Left button for uninstall on non-touch
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && count > 0) {
    mode_ = 1;
    updateRequired_ = true;
    return;
  }

  if (count > 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedIndex_ = (selectedIndex_ + count - 1) % count;
      updateRequired_ = true;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedIndex_ = (selectedIndex_ + 1) % count;
      updateRequired_ = true;
    }
  }

  // Install from inbox: Right key
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    xSemaphoreTake(renderingMutex_, portMAX_DELAY);
    exitActivity();
    enterNewActivity(new AppInstallActivity(renderer, mappedInput, "", [this]() {
      exitActivity();
      reload();
      updateRequired_ = true;
    }));
    xSemaphoreGive(renderingMutex_);
  }
}

void AppListActivity::render() const {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "应用");

  if (mode_ == 1 && !apps_.empty()) {
    const auto& a = apps_[static_cast<size_t>(selectedIndex_)];
    const auto dialog = uninstallDialogLayout(renderer);
    const auto toggle = uninstallDataToggleRect(renderer);
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 100, "卸载应用", true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 155, a.name.c_str());
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 195, "确认移除这个扩展应用？");

    renderer.fillRoundedRect(toggle.x, toggle.y, toggle.width, toggle.height, 10, Color::LightGray);
    M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, toggle.x, toggle.y, toggle.width, toggle.height,
                                uninstallClearData_ ? "同时清除数据：是" : "同时清除数据：否", true,
                                EpdFontFamily::REGULAR, 8);

    const auto drawDialogButton = [&](int index, const char* label) {
      const auto r = dialog.buttonRect(index);
      renderer.fillRoundedRect(r.x, r.y, r.width, r.height, 12, index == 1 ? Color::Black : Color::LightGray);
      M4UiText::drawCenteredInBox(renderer, UI_10_FONT_ID, r.x, r.y, r.width, r.height, label, index == 0,
                                  EpdFontFamily::BOLD, 8);
    };
    drawDialogButton(0, "取消");
    drawDialogButton(1, "确认卸载");
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, pageHeight - 90, "点按按钮操作；左右键切换数据选项");
  } else if (apps_.empty()) {
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, 200, "尚未安装扩展应用", true);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 260, "将 .m4x 拷到 /apps_inbox/");
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, 300, "点按屏幕或确认 安装");
  } else {
    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int count = static_cast<int>(apps_.size());
    GUI.drawList(
        renderer, Rect{0, listTop, pageWidth, listHeight}, count, selectedIndex_,
        [this](int i) { return apps_[static_cast<size_t>(i)].name; }, nullptr, nullptr,
        [this](int i) {
          return apps_[static_cast<size_t>(i)].version;
        });
  }

  const auto labels = mode_ == 1 ? mappedInput.mapLabels("« 返回", "确认", "", "")
                                 : mappedInput.mapLabels("« 返回", "打开", "卸载", "安装");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
