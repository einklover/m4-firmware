#pragma once

#include "apps/M4xManifest.h"

#include <string>
#include <vector>

struct M4xInstalledApp {
  std::string id;
  std::string name;
  std::string version;
  int versionCode = 0;
  std::string path;  // e.g. /apps/com.example.clock
  std::string entry;
  std::string icon;
  std::vector<std::string> permissions;
  // Installed package file inventory (manifest + entry + icon + files[]) for clean uninstall/upgrade.
  std::vector<std::string> files;
  uint32_t installedAt = 0;
};

class M4xRegistry {
 public:
  // Load registry from SD; empty if missing/corrupt.
  static std::vector<M4xInstalledApp> load();
  // Persist full list.
  static bool save(const std::vector<M4xInstalledApp>& apps);

  static const M4xInstalledApp* find(const std::vector<M4xInstalledApp>& apps, const std::string& id);

  // Upsert from installed manifest + install path.
  static void upsert(std::vector<M4xInstalledApp>& apps, const M4xManifest& m, const std::string& installPath,
                     uint32_t installedAt);

  static bool remove(std::vector<M4xInstalledApp>& apps, const std::string& id);
};
