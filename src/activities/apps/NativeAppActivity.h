#pragma once

#include "../ActivityWithSubactivity.h"
#include "apps/M4xRegistry.h"
#include "apps/native/M4NativeUi.h"
#include "apps/native/M4NativeUiController.h"

#include <functional>
#include <memory>
#include <string>

class NativeAppActivity final : public ActivityWithSubactivity {
 public:
  NativeAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, M4xInstalledApp app,
                    const std::function<void()>& onExitApp);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  std::string debugUiJson() override;

 private:
  bool loadDocument();
  const M4NativeUi::Screen* currentScreen() const;
  void render();
  void handleAction(const std::string& action, const M4NativeUi::Node* node = nullptr, int index0 = -1);
  bool rowAt(int index0, M4NativeUi::Row& out) const;
  std::string resolved(const std::string& s) const;
  void setError(const std::string& error);

  M4xInstalledApp app_;
  std::function<void()> onExitApp_;
  std::unique_ptr<M4NativeUi::Controller> controller_;
  M4NativeUi::Document document_;
  std::string screenId_;
  std::string error_;
  bool updateRequired_ = true;

  // One flex list per screen in v1. Other components remain fixed-height.
  int selectedIndex_ = 0;
  int tabIndex_ = 0;
  int listTop_ = 0;
  int listHeight_ = 0;
  int listCount_ = 0;
  std::string listSource_;
  std::string listNodeId_;
  std::string listAction_;
  std::string buttonActions_[4];
};
