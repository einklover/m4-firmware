#include "apps/native/M4NativeAppControllerFactory.h"

#include "apps/M4xJsonStream.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "apps/providers/M4NativeProviderManager.h"
#include "util/M4ContentProviderContract.h"

#include <SDCardManager.h>

#include <algorithm>
#include <utility>

namespace M4NativeAppControllers {
namespace {

bool fieldAt(const std::string& line, int field, std::string& out) {
  out.clear();
  int cur = 0;
  size_t start = 0;
  for (size_t i = 0; i <= line.size(); ++i) {
    if (i != line.size() && line[i] != '\t') continue;
    if (cur == field) {
      out.assign(line, start, i - start);
      return true;
    }
    ++cur;
    start = i + 1;
  }
  return false;
}

size_t countLines(const std::string& path) {
  FsFile f;
  if (!SdMan.openFileForRead("NA-SHELF", path.c_str(), f)) return 0;
  uint8_t buf[1024];
  size_t rows = 0;
  bool any = false;
  uint8_t last = '\n';
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    any = true;
    for (int i = 0; i < n; ++i) {
      last = buf[i];
      if (buf[i] == '\n') ++rows;
    }
  }
  f.close();
  if (any && last != '\n') ++rows;
  return rows;
}

bool readLineAt(const std::string& path, size_t target, std::string& line) {
  line.clear();
  FsFile f;
  if (!SdMan.openFileForRead("NA-SHELF", path.c_str(), f)) return false;
  uint8_t buf[1024];
  size_t row = 0;
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) {
      const char c = static_cast<char>(buf[i]);
      if (row == target) {
        if (c == '\n') {
          f.close();
          if (!line.empty() && line.back() == '\r') line.pop_back();
          return !line.empty();
        }
        if (line.size() >= 2048) {
          f.close();
          return false;
        }
        line.push_back(c);
      } else if (c == '\n') {
        ++row;
      }
    }
  }
  f.close();
  return row == target && !line.empty();
}

class ShelfSink final : public M4xJsonStream::Sink {
 public:
  ~ShelfSink() override { if (open_) f_.close(); }
  bool open(const std::string& path) {
    M4NativeProviderIo::ensureParentDirs(path);
    if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
    open_ = SdMan.openFileForWrite("NA-SHELF", path.c_str(), f_);
    return open_;
  }
  bool write(const uint8_t* data, size_t len) override {
    if (!open_ || !data) return false;
    return f_.write(data, len) == static_cast<int>(len);
  }
 private:
  FsFile f_;
  bool open_ = false;
};

bool projectLegacyShelf(const M4xInstalledApp& app, const std::string& rowsPath) {
  const std::string root = std::string("/apps_data/") + app.id;
  std::string source = root + "/shelf_cache.json";
  if (!SdMan.exists(source.c_str())) source = root + "/shelf.json";
  if (!SdMan.exists(source.c_str())) return false;
  FsFile in;
  if (!SdMan.openFileForRead("NA-SHELF", source.c_str(), in) || in.fileSize() > 2u * 1024u * 1024u) {
    if (in.isOpen()) in.close();
    return false;
  }
  ShelfSink sink;
  if (!sink.open(rowsPath)) {
    in.close();
    return false;
  }
  M4xJsonStream::RecordExtractor rows({"books"}, {"bookId", "title", "author", "progress"}, sink, 512);
  uint8_t buf[2048];
  bool ok = true;
  while (in.available()) {
    const int n = in.read(buf, sizeof(buf));
    if (n <= 0) { ok = false; break; }
    if (!rows.feed(buf, static_cast<size_t>(n))) { ok = false; break; }
  }
  in.close();
  return ok && rows.finish() && rows.recordCount() > 0;
}

class BaseController : public M4NativeUi::Controller {
 public:
  explicit BaseController(M4xInstalledApp app) : app_(std::move(app)) {}

  bool scalar(const std::string& key, std::string& out) const override {
    if (key == "app.name") out = app_.name;
    else if (key == "app.id") out = app_.id;
    else if (key == "app.version") out = app_.version;
    else if (key == "app.provider") out = app_.provider;
    else if (key == "runtime.status") out = "native";
    else {
      out.clear();
      return false;
    }
    return true;
  }

  M4NativeUi::ActionResult dispatch(const std::string& action,
                                    const M4NativeUi::ActionContext& ctx) override {
    (void)ctx;
    if (action == "system.back" || action == "system.close") return M4NativeUi::ActionResult::close();
    M4NativeUi::ActionResult r;
    r.kind = M4NativeUi::ActionKind::Error;
    r.error = action.empty() ? "empty_action" : "unsupported_action";
    return r;
  }

 protected:
  M4xInstalledApp app_;
};

class ProviderController final : public BaseController {
 public:
  explicit ProviderController(M4xInstalledApp app) : BaseController(std::move(app)) {
    root_ = std::string("/apps_data/") + app_.id;
    shelfRows_ = root_ + "/provider/shelf_rows.tsv";
    (void)projectLegacyShelf(app_, shelfRows_);
    shelfCount_ = countLines(shelfRows_);
  }

  bool scalar(const std::string& key, std::string& out) const override {
    if (BaseController::scalar(key, out)) return true;
    if (key == "page.status") {
      if (shelfCount_ == 0) out = "暂无本地书架 · 可从历史记录打开已缓存书籍";
      else out = std::string("书架 ") + std::to_string(shelfCount_) + " 本";
      return true;
    }
    out.clear();
    return false;
  }

  size_t rowCount(const std::string& source) const override {
    if (source == "provider.books" || source == "provider.shelf") return shelfCount_;
    return 0;
  }

  bool rowAt(const std::string& source, size_t index0, M4NativeUi::Row& out) const override {
    out = {};
    if ((source != "provider.books" && source != "provider.shelf") || index0 >= shelfCount_) return false;
    std::string line;
    if (!readLineAt(shelfRows_, index0, line)) return false;
    fieldAt(line, 0, out.key);
    fieldAt(line, 1, out.title);
    fieldAt(line, 2, out.subtitle);
    fieldAt(line, 3, out.value);
    if (out.title.empty()) out.title = out.key;
    if (!out.value.empty()) out.value += "%";
    return !out.key.empty();
  }

  M4NativeUi::ActionResult dispatch(const std::string& action,
                                    const M4NativeUi::ActionContext& ctx) override {
    if (action == "system.back" || action == "system.close") return M4NativeUi::ActionResult::close();
    if (action == "provider.openBook" || action == "provider.openSelected") {
      if (ctx.rowKey.empty()) {
        M4NativeUi::ActionResult r;
        r.kind = M4NativeUi::ActionKind::Error;
        r.error = "book_not_selected";
        return r;
      }
      std::string title;
      M4NativeUi::Row row;
      if (ctx.index0 >= 0 && rowAt(ctx.source.empty() ? "provider.books" : ctx.source,
                                  static_cast<size_t>(ctx.index0), row)) title = row.title;
      if (!M4NativeProviderManager::ensureBook(app_.provider, ctx.rowKey, app_.id, title)) {
        M4NativeUi::ActionResult r;
        r.kind = M4NativeUi::ActionKind::Error;
        r.error = "book_catalog_missing";
        return r;
      }
      return M4NativeUi::ActionResult::openProviderBook(
          M4ContentProvider::makeHistoryUri(app_.provider.c_str(), ctx.rowKey.c_str()));
    }
    if (action == "provider.login") {
      M4NativeUi::ActionResult r;
      r.kind = M4NativeUi::ActionKind::OpenLogin;
      r.payload = app_.provider;
      return r;
    }
    if (action == "provider.refresh") {
      M4NativeUi::ActionResult r;
      r.kind = M4NativeUi::ActionKind::Error;
      r.error = "native_discovery_not_migrated";
      return r;
    }
    return BaseController::dispatch(action, ctx);
  }

 private:
  std::string root_;
  std::string shelfRows_;
  size_t shelfCount_ = 0;
};

}  // namespace

std::unique_ptr<M4NativeUi::Controller> create(const M4xInstalledApp& app) {
  if (app.runtime == M4xRuntimeKind::Native && !app.provider.empty() &&
      M4NativeProviderManager::supports(app.provider)) {
    return std::make_unique<ProviderController>(app);
  }
  return std::make_unique<BaseController>(app);
}

}  // namespace M4NativeAppControllers
