//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include "headers/header.h"         // Declares Header, register_factory, create, getRegistry.
#include "headers/string_header.h"  // Declares StringHeader.

using namespace athenasip::headers;

// -----------------------------------------------------------------------------
// Helper accessor class to expose the protected registry.
// This subclass exists solely for testing purposes.
class HeaderTestAccessor : public Header {
 public:
  // Expose the registry publicly.
  static std::unordered_map<std::string, std::function<std::shared_ptr<Header>()>>& publicRegistry() {
    return Header::getRegistry();
  }
  // Provide dummy implementations to make this class instantiable.
  virtual bool parse(const std::string& /*value*/) override { return false; }
  virtual std::string to_string() const override { return ""; }
};

// -----------------------------------------------------------------------------
// DummyHeader: Implements parse() and to_string() so we can test custom factories.
class DummyHeader : public Header {
 public:
  std::string dummy;
  virtual bool parse(const std::string& value) override {
    dummy = "dummy:" + value;
    return true;
  }
  virtual std::string to_string() const override {
    return dummy;
  }
};

// FailHeader: A header whose parse() always fails.
class FailHeader : public Header {
 public:
  virtual bool parse(const std::string& /*value*/) override {
    return false;
  }
  virtual std::string to_string() const override {
    return "";
  }
};

TEST(HeaderFactoryTest, FallbackReturnsStringHeader) {
  // Clear the registry via our test accessor.
  HeaderTestAccessor::publicRegistry().clear();

  // When no factory is registered for "X-Unknown", create() should fall back to a default StringHeader.
  std::shared_ptr<Header> hdr = Header::create("X-Unknown", "fallback-test");
  auto sh = std::dynamic_pointer_cast<StringHeader>(hdr);
  ASSERT_NE(sh, nullptr) << "Expected fallback to StringHeader";
  // Instead of accessing a (possibly private) member variable, we use to_string().
  EXPECT_EQ(sh->to_string(), "fallback-test");
}

TEST(HeaderFactoryTest, CustomFactoryRegistration) {
  HeaderTestAccessor::publicRegistry().clear();

  // Register a custom factory for field "X-Custom" that returns a DummyHeader.
  Header::register_factory("X-Custom", []() -> std::shared_ptr<Header> {
    return std::make_shared<DummyHeader>();
  });

  // Create a header using the custom factory.
  std::shared_ptr<Header> hdr = Header::create("X-Custom", "custom-value");
  auto dh = std::dynamic_pointer_cast<DummyHeader>(hdr);
  ASSERT_NE(dh, nullptr) << "Expected a DummyHeader from custom factory";
  EXPECT_EQ(dh->dummy, "dummy:custom-value");
}

TEST(HeaderFactoryTest, CustomFactoryParseFailureFallsBack) {
  HeaderTestAccessor::publicRegistry().clear();

  // Register a factory for "X-Fail" that produces an instance whose parse() always fails.
  Header::register_factory("X-Fail", []() -> std::shared_ptr<Header> {
    return std::make_shared<FailHeader>();
  });

  // When create() is called, since parse() returns false, the fallback should be used.
  std::shared_ptr<Header> hdr = Header::create("X-Fail", "some-value");
  auto sh = std::dynamic_pointer_cast<StringHeader>(hdr);
  ASSERT_NE(sh, nullptr) << "Expected fallback to StringHeader when custom parse fails";
  EXPECT_EQ(sh->to_string(), "some-value");
}

TEST(HeaderFactoryTest, RegistryRetention) {
  HeaderTestAccessor::publicRegistry().clear();

  // Initially, the registry should be empty.
  EXPECT_TRUE(HeaderTestAccessor::publicRegistry().empty());

  // Register two factories.
  Header::register_factory("X-One", []() -> std::shared_ptr<Header> {
    return std::make_shared<StringHeader>();
  });
  Header::register_factory("X-Two", []() -> std::shared_ptr<Header> {
    return std::make_shared<DummyHeader>();
  });

  // Verify that the registry now contains two entries.
  EXPECT_EQ(HeaderTestAccessor::publicRegistry().size(), 2);
}
