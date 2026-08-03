#include "ActivityWithSubactivity.h"

void ActivityWithSubactivity::exitActivity() {
  if (subActivity) {
    subActivity->onExit();
    subActivity.reset();
  }
  pendingExitSub_ = false;
}

void ActivityWithSubactivity::enterNewActivity(Activity* activity) {
  // Replace any existing child safely (caller must not be inside child loop).
  exitActivity();
  subActivity.reset(activity);
  if (subActivity) subActivity->onEnter();
}

bool ActivityWithSubactivity::pumpSubActivityFrame() {
  if (!subActivity) return false;
  subActivity->loop();
  if (pendingExitSub_) {
    pendingExitSub_ = false;
    exitActivity();
    return true;
  }
  return false;
}

void ActivityWithSubactivity::loop() {
  if (subActivity) {
    pumpSubActivityFrame();
  }
}

void ActivityWithSubactivity::onExit() {
  Activity::onExit();
  // Only call onExit on subActivity, do NOT reset (free) it here.
  // The destructor will handle cleanup. This prevents use-after-free when
  // onExit is triggered from within subActivity's own call stack
  // (e.g., reader back button → goToLibrary → global::exitActivity).
  if (subActivity) {
    subActivity->onExit();
  }
}
