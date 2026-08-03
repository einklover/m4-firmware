#pragma once

#include <string>
#include <vector>

// Parsed install-package manifest (manifest.json inside .m4x).
struct M4xManifest {
  std::string id;           // unique package id, e.g. com.example.clock
  std::string name;         // display name
  std::string version;      // human version string
  int versionCode = 0;      // integer for upgrade compare
  std::string minFirmware;  // optional
  std::string author;
  std::string entry = "main.lua";
  std::string icon;  // relative path inside package
  std::string description;
  std::vector<std::string> permissions;
  // Explicit extra package files the installer may extract (sys.load modules, assets).
  // Always extracted in addition: manifest.json, entry, optional icon.
  std::vector<std::string> files;

  bool valid = false;
  std::string error;
};

// Parse JSON text into manifest; sets valid/error.
// Also validates entry/icon/files package-relative paths (M4xPathSafe).
M4xManifest M4xParseManifest(const char* json, size_t len);

// Validate package id: reverse-DNS-ish [a-z0-9_.-] with at least one dot.
bool M4xIsValidPackageId(const std::string& id);

// Permission is in the system allow-list.
bool M4xIsAllowedPermission(const std::string& perm);
