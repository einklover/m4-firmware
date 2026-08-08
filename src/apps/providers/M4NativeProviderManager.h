#pragma once

#include "apps/providers/M4NativeProvider.h"
#include "util/M4ContentProviderContract.h"

#include <string>

namespace M4NativeProviderManager {

bool supports(const std::string& providerId);

// Register + persist a book handoff. FileRows catalogs remain on SD; this only
// persists the descriptor needed for history/cold reopen without Lua.
bool registerBook(const M4ContentProvider::BookSpec& spec);

// Load persistent metadata, or infer legacy plugin metadata from
// /apps_data/<app>/cache/<book>/toc_catalog.json. appId/title may be empty.
bool ensureBook(const std::string& providerId, const std::string& bookId,
                const std::string& appId = {}, const std::string& title = {});

// Enqueue one chapter on the native single-flight worker. Returns true for
// already-ready cache too. Never starts Lua.
bool ensureChapter(const std::string& providerId, const std::string& bookId, int index0,
                   bool foreground = true);

M4NativeProvider::Progress progress();
void cancelForeground();

// Absolute app-data root and cache path helpers for native history/reader.
std::string appDataRootFor(const std::string& providerId, const std::string& bookId);
std::string chapterRelPath(const std::string& bookId, const std::string& chapterUid);

// Start worker lazily; safe to call repeatedly.
void begin();

}  // namespace M4NativeProviderManager
