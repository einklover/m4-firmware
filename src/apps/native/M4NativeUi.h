#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace M4NativeUi {

inline constexpr int kSchemaVersion = 1;

struct Limits {
  size_t maxBytes = 32u * 1024u;
  size_t maxScreens = 12;
  size_t maxNodesPerScreen = 48;
  size_t maxAttrBytes = 512;
  size_t maxStringBytes = 256;
};

enum class NodeType : uint8_t {
  Text = 0,
  FlowText,
  List,
  Tabs,
  Progress,
  Spacer,
  Divider,
  Buttons,
};

struct Node {
  NodeType type = NodeType::Text;
  std::string id;
  std::string text;       // literal text; @foo means scalar binding
  std::string source;     // list/tab/flow datasource
  std::string titleField; // list row title field
  std::string subtitleField;
  std::string valueField;
  std::string action;     // activate/change/tap action
  std::string secondaryAction;
  int height = 0;
  int pageSize = 0;
  int value = 0;
  int max = 0;
  bool selectable = true;
  bool bold = false;

  // Button-bar labels/actions. Slots map to Back/Confirm/Left/Right.
  std::string labels[4];
  std::string actions[4];
};

struct Screen {
  std::string id;
  std::string title;      // literal; @foo means scalar binding
  std::vector<Node> nodes;
};

struct Document {
  int version = kSchemaVersion;
  std::string startScreen;
  std::vector<Screen> screens;
};

enum class ParseError : uint8_t {
  None = 0,
  Empty,
  TooLarge,
  Syntax,
  UnsupportedVersion,
  BadRoot,
  BadScreen,
  BadNode,
  TooManyScreens,
  TooManyNodes,
  AttributeTooLong,
  StringTooLong,
  DuplicateScreen,
  MissingStartScreen,
};

const char* errorKey(ParseError e);

struct ParseResult {
  Document document;
  ParseError error = ParseError::None;
  size_t offset = 0;
  std::string detail;
  explicit operator bool() const { return error == ParseError::None; }
};

// Parse the bounded M4 Native UI XML subset. This is deliberately not a full
// XML implementation: no DTD/entities/namespaces/CDATA, no executable
// expressions, and UI component nodes must be self-closing. The restricted
// grammar makes package parsing deterministic and small enough for ESP32-S3.
ParseResult parse(const char* xml, size_t len, const Limits& limits = {});

const Screen* findScreen(const Document& doc, const std::string& id);

}  // namespace M4NativeUi
