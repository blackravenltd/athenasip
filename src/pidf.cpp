//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "pidf.h"

#include <cctype>
#include <optional>
#include <utility>

namespace athenasip::pidf {

namespace {

constexpr const char* kNamespace = "urn:ietf:params:xml:ns:pidf";
constexpr std::size_t kNone = std::string::npos;

std::string escape(const std::string& text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

// A start tag: its name, its attributes in order, where it ends, and whether it closes itself.
struct Tag {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::size_t end = kNone;  // one past the '>'
  bool empty = false;
};

bool is_name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == ':' || c == '_' || c == '-' || c == '.'; }

// The start tag beginning at `at`, which is a '<'.
std::optional<Tag> read_tag(const std::string& text, std::size_t at) {
  Tag tag;
  std::size_t i = at + 1;
  while (i < text.size() && is_name_char(text[i])) tag.name += text[i++];
  if (tag.name.empty()) return std::nullopt;

  while (i < text.size()) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i >= text.size()) return std::nullopt;
    if (text[i] == '>') {
      tag.end = i + 1;
      return tag;
    }
    if (text.compare(i, 2, "/>") == 0) {
      tag.end = i + 2;
      tag.empty = true;
      return tag;
    }

    std::string name;
    while (i < text.size() && is_name_char(text[i])) name += text[i++];
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (name.empty() || i >= text.size() || text[i] != '=') return std::nullopt;
    ++i;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i >= text.size() || (text[i] != '"' && text[i] != '\'')) return std::nullopt;
    const char quote = text[i++];
    const auto close = text.find(quote, i);
    if (close == kNone) return std::nullopt;
    tag.attributes.emplace_back(std::move(name), text.substr(i, close - i));
    i = close + 1;
  }
  return std::nullopt;
}

// Past a comment, CDATA section, processing instruction or declaration starting at `at`; kNone if it is none of
// them or does not end.
std::optional<std::size_t> skip_markup(const std::string& text, std::size_t at) {
  auto past = [&text, at](const char* open, const char* close) -> std::optional<std::size_t> {
    if (text.compare(at, std::char_traits<char>::length(open), open) != 0) return std::nullopt;
    const auto end = text.find(close, at + std::char_traits<char>::length(open));
    return end == kNone ? kNone : end + std::char_traits<char>::length(close);
  };
  if (auto end = past("<!--", "-->")) return end;
  if (auto end = past("<![CDATA[", "]]>")) return end;
  if (auto end = past("<?", "?>")) return end;
  if (auto end = past("<!", ">")) return end;
  return std::nullopt;
}

// One past the end of the element whose start tag is `tag`, counting the elements inside it; kNone if it never ends.
std::size_t element_end(const std::string& text, const Tag& tag) {
  if (tag.empty) return tag.end;

  std::size_t depth = 1;
  std::size_t i = tag.end;
  while (true) {
    i = text.find('<', i);
    if (i == kNone) return kNone;

    if (auto skipped = skip_markup(text, i)) {
      if (*skipped == kNone) return kNone;
      i = *skipped;
    } else if (text.compare(i, 2, "</") == 0) {
      const auto close = text.find('>', i);
      if (close == kNone) return kNone;
      i = close + 1;
      if (--depth == 0) return i;
    } else {
      auto inner = read_tag(text, i);
      if (!inner) return kNone;
      if (!inner->empty) ++depth;
      i = inner->end;
    }
  }
}

std::string local_name(const std::string& name) {
  const auto colon = name.find(':');
  return colon == kNone ? name : name.substr(colon + 1);
}

std::string attribute(const Tag& tag, const std::string& name) {
  for (const auto& [key, value] : tag.attributes) {
    if (key == name) return value;
  }
  return "";
}

struct Element {
  std::string key;  // the name and id that make two the same element; empty when it has no id
  std::string name;
  std::string text;
};

struct Document {
  std::vector<std::pair<std::string, std::string>> namespaces;  // xmlns:prefix declarations on the root
  std::vector<Element> elements;
};

// RFC 3863 4.1: a <presence> root in the PIDF namespace, and its children whole.
std::optional<Document> read(const std::string& text) {
  std::size_t i = 0;
  std::optional<Tag> root;
  while (!root) {
    i = text.find('<', i);
    if (i == kNone) return std::nullopt;
    if (auto skipped = skip_markup(text, i)) {
      if (*skipped == kNone) return std::nullopt;
      i = *skipped;
      continue;
    }
    root = read_tag(text, i);
    if (!root) return std::nullopt;
  }

  // The root's own prefix, if it has one, names the PIDF namespace.
  const auto colon = root->name.find(':');
  const auto prefix = colon == kNone ? std::string() : root->name.substr(0, colon);
  if (local_name(root->name) != "presence" || attribute(*root, prefix.empty() ? "xmlns" : "xmlns:" + prefix) != kNamespace) return std::nullopt;

  Document document;
  for (const auto& [key, value] : root->attributes) {
    if (key.rfind("xmlns:", 0) == 0) document.namespaces.emplace_back(key, value);
  }
  if (root->empty) return document;

  i = root->end;
  while (true) {
    i = text.find('<', i);
    if (i == kNone) return std::nullopt;

    if (auto skipped = skip_markup(text, i)) {
      if (*skipped == kNone) return std::nullopt;
      i = *skipped;
      continue;
    }
    if (text.compare(i, 2, "</") == 0) return document;

    auto tag = read_tag(text, i);
    if (!tag) return std::nullopt;
    const auto end = element_end(text, *tag);
    if (end == kNone) return std::nullopt;

    Element element;
    element.name = local_name(tag->name);
    const auto id = attribute(*tag, "id");
    if (!id.empty()) element.key = tag->name + "#" + id;
    element.text = text.substr(i, end - i);
    document.elements.push_back(std::move(element));
    i = end;
  }
}

std::string header(const std::string& entity, const std::vector<std::pair<std::string, std::string>>& namespaces) {
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<presence xmlns=\"" + std::string(kNamespace) + "\"";
  for (const auto& [key, value] : namespaces) out += " " + key + "=\"" + value + "\"";
  return out + " entity=\"" + escape(entity) + "\"";
}

}  // namespace

std::string compose(const std::string& entity, const std::vector<std::string>& documents) {
  std::vector<std::pair<std::string, std::string>> namespaces;
  std::vector<Element> elements;

  for (const auto& text : documents) {
    auto document = read(text);
    if (!document) continue;

    // A prefix keeps the first namespace it was declared for.
    for (auto& declared : document->namespaces) {
      bool known = false;
      for (const auto& existing : namespaces) known = known || existing.first == declared.first;
      if (!known) namespaces.push_back(std::move(declared));
    }

    for (auto& element : document->elements) {
      if (!element.key.empty()) std::erase_if(elements, [&element](const Element& earlier) { return earlier.key == element.key; });
      elements.push_back(std::move(element));
    }
  }

  // RFC 3863 4.1: tuples, then notes, then anything else.
  std::string tuples, notes, others;
  for (const auto& element : elements) {
    if (element.name == "tuple") {
      tuples += element.text;
    } else if (element.name == "note") {
      notes += element.text;
    } else {
      others += element.text;
    }
  }

  const auto body = tuples + notes + others;
  return header(entity, namespaces) + (body.empty() ? "/>\r\n" : ">" + body + "</presence>\r\n");
}

std::string registration(const std::string& entity, bool registered) {
  return header(entity, {}) + "><tuple id=\"registration\"><status><basic>" + (registered ? "open" : "closed") + "</basic></status></tuple></presence>\r\n";
}

}  // namespace athenasip::pidf
