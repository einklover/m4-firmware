#include "apps/native/M4NativeAppControllerFactory.h"

#include <utility>

namespace M4NativeAppControllers {
namespace {

class BaseController final : public M4NativeUi::Controller {
 public:
  explicit BaseController(M4xInstalledApp app) : app_(std::move(app)) {}

  bool scalar(const std::string& key, std::string& out) const override {
    if (key == "app.name") out = app_.name;
    else if (key == "app.id") out = app_.id;
    else if (key == "app.version") out = app_.version;
    else if (key == "app.provider") out = app_.provider;
    else if (key == "runtime.status") out = app_.provider.empty() ? "native" : "provider adapter unavailable";
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

 private:
  M4xInstalledApp app_;
};

}  // namespace

std::unique_ptr<M4NativeUi::Controller> create(const M4xInstalledApp& app) {
  return std::make_unique<BaseController>(app);
}

}  // namespace M4NativeAppControllers
