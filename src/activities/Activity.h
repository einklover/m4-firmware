#pragma once

#include <HardwareSerial.h>

#include <string>
#include <utility>

#include "util/M4TouchNavigation.h"

class MappedInputManager;
class GfxRenderer;

class Activity {
 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter() {
    M4TouchNavigation::activateForActivity(showTouchNavigation());
    Serial.printf("[%lu] [ACT] Entering activity: %s\n", millis(), name.c_str());
  }
  virtual void onExit() {
    M4TouchNavigation::setMode(M4TouchNavigation::Mode::None);
    Serial.printf("[%lu] [ACT] Exiting activity: %s\n", millis(), name.c_str());
  }
  virtual void loop() {}
  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }
  virtual bool isHomeActivity() const { return false; }
  // Reader body and Home intentionally stay visually clean. Every other normal
  // activity gets explicit touch navigation unless it is a special boot/error
  // surface that overrides this policy.
  virtual bool showTouchNavigation() const { return !isReaderActivity() && !isHomeActivity(); }
  // Structured UI dump for m4adb `ui` (JSON object body, no outer braces required
  // to be complete alone — implementors return a full JSON object string).
  // Used for automation without OCR/screenshot text recognition.
  virtual std::string debugUiJson() { return "{}"; }
  const std::string& getName() const { return name; }
};
