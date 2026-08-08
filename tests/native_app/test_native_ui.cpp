#include "apps/native/M4NativeUi.h"
#include "apps/native/M4NativeUiController.h"
#include "util/M4ContentProviderContract.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {

class FixtureController final : public M4NativeUi::Controller {
 public:
  bool scalar(const std::string& key, std::string& out) const override {
    if (key == "app.name") {
      out = "Reader";
      return true;
    }
    return false;
  }
};

void parseHappyPath() {
  const char* xml = R"XML(<?xml version="1.0"?>
<m4ui version="1" start="shelf">
  <screen id="shelf" title="@app.name">
    <text text="Native &amp; bounded" bold="true"/>
    <list id="books" source="provider.books" titleField="title" subtitleField="author" onActivate="provider.openBook"/>
    <progress source="@loading.percent" max="100"/>
    <buttons back="Back" primary="Open" onBack="system.back" onPrimary="provider.openSelected"/>
  </screen>
</m4ui>)XML";
  const auto r = M4NativeUi::parse(xml, std::char_traits<char>::length(xml));
  assert(r);
  assert(r.document.startScreen == "shelf");
  const auto* s = M4NativeUi::findScreen(r.document, "shelf");
  assert(s != nullptr);
  assert(s->nodes.size() == 4);
  assert(s->nodes[0].text == "Native & bounded");
  assert(s->nodes[1].source == "provider.books");
  FixtureController c;
  assert(M4NativeUi::resolveText(c, s->title) == "Reader");
}

void rejectExecutableShape() {
  // Text nodes / script-like nested content are outside the M4 UI grammar.
  const char* xml = R"XML(<m4ui version="1"><screen id="x"><text>run()</text></screen></m4ui>)XML";
  const auto r = M4NativeUi::parse(xml, std::char_traits<char>::length(xml));
  assert(!r);
}

void rejectOversizeAndBadSource() {
  M4NativeUi::Limits lim;
  lim.maxBytes = 8;
  const char* xml = "<m4ui/>";
  assert(!M4NativeUi::parse(xml, std::char_traits<char>::length(xml), lim));

  const char* noSource = R"XML(<m4ui version="1"><screen id="x"><list id="l"/></screen></m4ui>)XML";
  assert(!M4NativeUi::parse(noSource, std::char_traits<char>::length(noSource)));
}

void providerContract() {
  const std::string uri = M4ContentProvider::makeHistoryUri("weread", "12345");
  assert(uri == "m4cp://weread/12345");
  std::string p, b;
  assert(M4ContentProvider::parseHistoryUri(uri.c_str(), p, b));
  assert(p == "weread" && b == "12345");
  assert(!M4ContentProvider::isSafeCacheRelPath("../secret.txt"));
  assert(M4ContentProvider::isSafeCacheRelPath("cache/123/ch_1.txt"));
}

}  // namespace

int main() {
  parseHappyPath();
  rejectExecutableShape();
  rejectOversizeAndBadSource();
  providerContract();
  std::cout << "native-ui tests passed\n";
  return 0;
}
