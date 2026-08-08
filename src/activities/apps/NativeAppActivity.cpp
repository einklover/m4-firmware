#include "NativeAppActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>

#include "MappedInputManager.h"
#include "apps/native/M4NativeAppControllerFactory.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4UiText.h"
#include "util/TouchHitGeometry.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {

void* uiAlloc(size_t n) {
#if defined(ARDUINO_ARCH_ESP32)
  if (n > 0) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
  }
#endif
  return std::malloc(n);
}

void uiFree(void* p) { std::free(p); }

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(c));
      out += b;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

}  // namespace

NativeAppActivity::NativeAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     M4xInstalledApp app, const std::function<void()>& onExitApp)
    : ActivityWithSubactivity("NativeApp", renderer, mappedInput),
      app_(std::move(app)),
      onExitApp_(onExitApp),
      controller_(M4NativeAppControllers::create(app_)) {}

void NativeAppActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  error_.clear();
  if (!loadDocument()) {
    updateRequired_ = true;
    return;
  }
  screenId_ = document_.startScreen;
  selectedIndex_ = 0;
  tabIndex_ = 0;
  updateRequired_ = true;
}

void NativeAppActivity::onExit() {
  ActivityWithSubactivity::onExit();
  controller_.reset();
  document_ = {};
}

bool NativeAppActivity::loadDocument() {
  std::string path = app_.path;
  if (!path.empty() && path.back() != '/') path += '/';
  path += app_.entry.empty() ? "main.xml" : app_.entry;

  FsFile f;
  if (!SdMan.openFileForRead("NativeUI", path.c_str(), f)) {
    setError("ui_entry_missing");
    return false;
  }
  const size_t n = f.fileSize();
  M4NativeUi::Limits lim;
  if (n == 0 || n > lim.maxBytes) {
    f.close();
    setError(n == 0 ? "ui_entry_empty" : "ui_entry_too_large");
    return false;
  }
  char* buf = static_cast<char*>(uiAlloc(n + 1));
  if (!buf) {
    f.close();
    setError("ui_oom");
    return false;
  }
  size_t off = 0;
  while (off < n) {
    const int r = f.read(reinterpret_cast<uint8_t*>(buf + off), n - off);
    if (r <= 0) break;
    off += static_cast<size_t>(r);
  }
  f.close();
  if (off != n) {
    uiFree(buf);
    setError("ui_read_failed");
    return false;
  }
  buf[n] = '\0';
  auto parsed = M4NativeUi::parse(buf, n, lim);
  uiFree(buf);
  if (!parsed) {
    setError(std::string("ui_parse:") + M4NativeUi::errorKey(parsed.error));
    return false;
  }
  document_ = std::move(parsed.document);
  return true;
}

const M4NativeUi::Screen* NativeAppActivity::currentScreen() const {
  return M4NativeUi::findScreen(document_, screenId_);
}

std::string NativeAppActivity::resolved(const std::string& s) const {
  return controller_ ? M4NativeUi::resolveText(*controller_, s) : s;
}

void NativeAppActivity::setError(const std::string& error) {
  error_ = error;
  updateRequired_ = true;
}

bool NativeAppActivity::rowAt(int index0, M4NativeUi::Row& out) const {
  if (!controller_ || index0 < 0 || index0 >= listCount_ || listSource_.empty()) return false;
  return controller_->rowAt(listSource_, static_cast<size_t>(index0), out);
}

void NativeAppActivity::handleAction(const std::string& action, const M4NativeUi::Node* node, int index0) {
  if (action.empty() || !controller_) return;
  M4NativeUi::ActionContext ctx;
  ctx.screenId = screenId_;
  if (node) {
    ctx.nodeId = node->id;
    ctx.source = node->source;
  } else {
    ctx.nodeId = listNodeId_;
    ctx.source = listSource_;
  }
  ctx.index0 = index0;
  if (index0 >= 0) {
    M4NativeUi::Row row;
    if (rowAt(index0, row)) ctx.rowKey = row.key;
  }
  const auto r = controller_->dispatch(action, ctx);
  switch (r.kind) {
    case M4NativeUi::ActionKind::Repaint:
      updateRequired_ = true;
      break;
    case M4NativeUi::ActionKind::Navigate:
      if (M4NativeUi::findScreen(document_, r.screenId)) {
        screenId_ = r.screenId;
        selectedIndex_ = 0;
        tabIndex_ = 0;
        updateRequired_ = true;
      } else {
        setError("ui_bad_route");
      }
      break;
    case M4NativeUi::ActionKind::Close:
      onExitApp_();
      break;
    case M4NativeUi::ActionKind::OpenProviderBook:
    case M4NativeUi::ActionKind::OpenProviderToc:
    case M4NativeUi::ActionKind::OpenLogin:
      // Native ProviderManager consumes these routes in the next layer. Never
      // fall back to Lua/network from the renderer itself.
      setError("provider_route_not_bound");
      break;
    case M4NativeUi::ActionKind::Error:
      setError(r.error.empty() ? "native_action_failed" : r.error);
      break;
    case M4NativeUi::ActionKind::None:
    default:
      break;
  }
}

void NativeAppActivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
    return;
  }
  if (updateRequired_) {
    updateRequired_ = false;
    render();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
    if (!buttonActions_[0].empty()) handleAction(buttonActions_[0]);
    else onExitApp_();
    return;
  }

  if (!error_.empty()) {
    int tx = 0, ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      onExitApp_();
    }
    return;
  }

  if (listCount_ > 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedIndex_ = (selectedIndex_ + listCount_ - 1) % listCount_;
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedIndex_ = (selectedIndex_ + 1) % listCount_;
      updateRequired_ = true;
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      handleAction(!listAction_.empty() ? listAction_ : buttonActions_[1], nullptr, selectedIndex_);
      return;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !buttonActions_[1].empty()) {
    handleAction(buttonActions_[1]);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && !buttonActions_[2].empty()) {
    handleAction(buttonActions_[2]);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && !buttonActions_[3].empty()) {
    handleAction(buttonActions_[3]);
    return;
  }

  if (mappedInput.hasTouch()) {
    int tx = 0, ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const auto metrics = UITheme::getInstance().getMetrics();
      const int footerTop = renderer.getScreenHeight() - metrics.buttonHintsHeight;
      if (ty >= footerTop) {
        const int slot = std::min(3, std::max(0, tx * 4 / std::max(1, renderer.getScreenWidth())));
        if (!buttonActions_[slot].empty()) handleAction(buttonActions_[slot]);
        return;
      }
      if (listCount_ > 0) {
        int hit = -1;
        const int rowStep = metrics.listWithSubtitleRowHeight;
        if (TouchHitGeometry::listIndexFromPoint(ty, listTop_, listHeight_, rowStep, listCount_, selectedIndex_, hit)) {
          selectedIndex_ = hit;
          updateRequired_ = true;
          handleAction(listAction_, nullptr, hit);
          return;
        }
      }
    }
  }
}

void NativeAppActivity::render() {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  for (auto& a : buttonActions_) a.clear();
  listTop_ = listHeight_ = listCount_ = 0;
  listSource_.clear();
  listNodeId_.clear();
  listAction_.clear();

  if (!error_.empty()) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, app_.name.c_str());
    M4UiText::drawCentered(renderer, UI_12_FONT_ID, h / 2 - 30, "页面加载失败", true, EpdFontFamily::BOLD);
    M4UiText::drawCentered(renderer, UI_10_FONT_ID, h / 2 + 20, error_.c_str());
    const auto labels = mappedInput.mapLabels("« 返回", "退出", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const auto* screen = currentScreen();
  if (!screen || !controller_) {
    setError("ui_screen_missing");
    return;
  }

  const std::string title = resolved(screen->title.empty() ? "@app.name" : screen->title);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, w, metrics.headerHeight}, title.c_str());

  const int footerH = metrics.buttonHintsHeight;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = h - footerH - metrics.verticalSpacing;

  int fixed = 0;
  for (const auto& n : screen->nodes) {
    switch (n.type) {
      case M4NativeUi::NodeType::Text: fixed += n.height > 0 ? n.height : 34; break;
      case M4NativeUi::NodeType::FlowText: fixed += n.height > 0 ? n.height : 96; break;
      case M4NativeUi::NodeType::Tabs: fixed += n.height > 0 ? n.height : metrics.tabBarHeight; break;
      case M4NativeUi::NodeType::Progress: fixed += n.height > 0 ? n.height : 34; break;
      case M4NativeUi::NodeType::Spacer: fixed += std::max(0, n.height); break;
      case M4NativeUi::NodeType::Divider: fixed += n.height > 0 ? n.height : 12; break;
      case M4NativeUi::NodeType::Buttons:
      case M4NativeUi::NodeType::List:
        break;
    }
  }

  const M4NativeUi::Node* flexList = nullptr;
  for (const auto& n : screen->nodes) {
    if (n.type == M4NativeUi::NodeType::List && !flexList) {
      flexList = &n;
      break;
    }
  }
  const int flexHeight = flexList ? std::max(metrics.listWithSubtitleRowHeight, contentBottom - y - fixed) : 0;

  for (const auto& n : screen->nodes) {
    if (y >= contentBottom && n.type != M4NativeUi::NodeType::Buttons) break;
    switch (n.type) {
      case M4NativeUi::NodeType::Text: {
        const int nh = n.height > 0 ? n.height : 34;
        const std::string text = resolved(n.text);
        M4UiText::draw(renderer, UI_12_FONT_ID, metrics.contentSidePadding, y + 4, text.c_str(), true,
                       n.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
        y += nh;
        break;
      }
      case M4NativeUi::NodeType::FlowText: {
        const int nh = n.height > 0 ? n.height : 96;
        const std::string text = resolved(n.text.empty() ? n.source : n.text);
        // v1 flow surface keeps the XML stable while typography is delegated
        // to the system. A shared EPUB line-break adapter can replace this
        // implementation without changing package/controller contracts.
        size_t start = 0;
        int lineY = y + 2;
        while (start <= text.size() && lineY < y + nh) {
          const size_t nl = text.find('\n', start);
          const size_t end = nl == std::string::npos ? text.size() : nl;
          const std::string logical = text.substr(start, end - start);
          const std::string line = M4UiText::truncated(renderer, UI_12_FONT_ID, logical.c_str(),
                                                       w - 2 * metrics.contentSidePadding);
          M4UiText::draw(renderer, UI_12_FONT_ID, metrics.contentSidePadding, lineY, line.c_str());
          lineY += 28;
          if (nl == std::string::npos) break;
          start = nl + 1;
        }
        y += nh;
        break;
      }
      case M4NativeUi::NodeType::Tabs: {
        const int nh = n.height > 0 ? n.height : metrics.tabBarHeight;
        const size_t count = std::min<size_t>(12, controller_->rowCount(n.source));
        std::vector<M4NativeUi::Row> rows(count);
        std::vector<TabInfo> tabs;
        tabs.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          controller_->rowAt(n.source, i, rows[i]);
          tabs.push_back({rows[i].title.c_str(), static_cast<int>(i) == tabIndex_});
        }
        GUI.drawTabBar(renderer, Rect{0, y, w, nh}, tabs, true);
        y += nh;
        break;
      }
      case M4NativeUi::NodeType::List: {
        if (&n != flexList) break;
        listTop_ = y;
        listHeight_ = flexHeight;
        listSource_ = n.source;
        listNodeId_ = n.id;
        listAction_ = n.action;
        listCount_ = static_cast<int>(std::min<size_t>(200000, controller_->rowCount(n.source)));
        if (listCount_ <= 0) {
          M4UiText::drawCentered(renderer, UI_10_FONT_ID, y + 40, "暂无内容");
        } else {
          if (selectedIndex_ >= listCount_) selectedIndex_ = listCount_ - 1;
          if (selectedIndex_ < 0) selectedIndex_ = 0;
          int cacheIndex = -1;
          M4NativeUi::Row cache;
          auto get = [&](int i) -> const M4NativeUi::Row& {
            if (cacheIndex != i) {
              cache = {};
              controller_->rowAt(n.source, static_cast<size_t>(i), cache);
              cacheIndex = i;
            }
            return cache;
          };
          GUI.drawList(renderer, Rect{0, y, w, flexHeight}, listCount_, selectedIndex_,
                       [&](int i) { return get(i).title; },
                       [&](int i) { return get(i).subtitle; }, nullptr,
                       [&](int i) { return get(i).value; });
        }
        y += flexHeight;
        break;
      }
      case M4NativeUi::NodeType::Progress: {
        const int nh = n.height > 0 ? n.height : 34;
        int value = n.value;
        int max = n.max > 0 ? n.max : 100;
        if (!n.source.empty() && M4NativeUi::isBinding(n.source)) {
          int bound = value;
          if (controller_->number(n.source.substr(1), bound)) value = bound;
        }
        value = std::max(0, std::min(max, value));
        GUI.drawProgressBar(renderer,
                            Rect{metrics.contentSidePadding, y + 10, w - 2 * metrics.contentSidePadding, 10},
                            static_cast<size_t>(value), static_cast<size_t>(std::max(1, max)));
        y += nh;
        break;
      }
      case M4NativeUi::NodeType::Spacer:
        y += std::max(0, n.height);
        break;
      case M4NativeUi::NodeType::Divider:
        renderer.fillRect(metrics.contentSidePadding, y + 4, w - 2 * metrics.contentSidePadding, 1, true);
        y += n.height > 0 ? n.height : 12;
        break;
      case M4NativeUi::NodeType::Buttons:
        for (int i = 0; i < 4; ++i) buttonActions_[i] = n.actions[i];
        break;
    }
  }

  const M4NativeUi::Node* buttons = nullptr;
  for (const auto& n : screen->nodes) {
    if (n.type == M4NativeUi::NodeType::Buttons) {
      buttons = &n;
      break;
    }
  }
  const char* raw[4] = {"« 返回", listCount_ > 0 ? "打开" : "", "", ""};
  std::string labelsOwned[4];
  if (buttons) {
    for (int i = 0; i < 4; ++i) {
      labelsOwned[i] = resolved(buttons->labels[i]);
      if (!labelsOwned[i].empty()) raw[i] = labelsOwned[i].c_str();
    }
  }
  const auto labels = mappedInput.mapLabels(raw[0], raw[1], raw[2], raw[3]);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

std::string NativeAppActivity::debugUiJson() {
  return "{\"kind\":\"native_app\",\"app_id\":\"" + jsonEscape(app_.id) +
         "\",\"provider\":\"" + jsonEscape(app_.provider) + "\",\"screen\":\"" +
         jsonEscape(screenId_) + "\",\"selected\":" + std::to_string(selectedIndex_) +
         ",\"rows\":" + std::to_string(listCount_) + ",\"error\":\"" + jsonEscape(error_) + "\"}";
}
