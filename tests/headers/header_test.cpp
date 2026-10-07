//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include "headers/header.h"
#include "headers/string_header.h"

using namespace athenasip::headers;

// Exposes the protected factory registry to the tests.
class HeaderTestAccessor : public Header {
 public:
  static std::unordered_map<std::string, std::function<std::shared_ptr<Header>()>>& publicRegistry() {
    return Header::get_registry();
  }
  virtual bool parse(const std::string& /*value*/) override { return false; }
  virtual std::string to_string() const override { return ""; }
};

// The registry is process-wide, so each test restores it. Left cleared, every later test in the process would
// parse To, Via and Authorization as plain StringHeaders.
class HeaderFactoryTest : public ::testing::Test {
 protected:
  void SetUp() override { _saved = HeaderTestAccessor::publicRegistry(); }

  void TearDown() override { HeaderTestAccessor::publicRegistry() = _saved; }

 private:
  std::unordered_map<std::string, std::function<std::shared_ptr<Header>()>> _saved;
};

// Parses and serialises, for testing custom factories.
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

TEST_F(HeaderFactoryTest, FallbackReturnsStringHeader) {
  HeaderTestAccessor::publicRegistry().clear();

  // With no factory registered, create() falls back to StringHeader.
  std::shared_ptr<Header> hdr = Header::create("X-Unknown", "fallback-test");
  auto sh = std::dynamic_pointer_cast<StringHeader>(hdr);
  ASSERT_NE(sh, nullptr) << "Expected fallback to StringHeader";
  EXPECT_EQ(sh->to_string(), "fallback-test");
}

TEST_F(HeaderFactoryTest, CustomFactoryRegistration) {
  HeaderTestAccessor::publicRegistry().clear();

  Header::register_factory("X-Custom", []() -> std::shared_ptr<Header> {
    return std::make_shared<DummyHeader>();
  });

  std::shared_ptr<Header> hdr = Header::create("X-Custom", "custom-value");
  auto dh = std::dynamic_pointer_cast<DummyHeader>(hdr);
  ASSERT_NE(dh, nullptr) << "Expected a DummyHeader from custom factory";
  EXPECT_EQ(dh->dummy, "dummy:custom-value");
}

TEST_F(HeaderFactoryTest, CustomFactoryParseFailureFallsBack) {
  HeaderTestAccessor::publicRegistry().clear();

  Header::register_factory("X-Fail", []() -> std::shared_ptr<Header> {
    return std::make_shared<FailHeader>();
  });

  // parse() fails, so create() falls back to StringHeader.
  std::shared_ptr<Header> hdr = Header::create("X-Fail", "some-value");
  auto sh = std::dynamic_pointer_cast<StringHeader>(hdr);
  ASSERT_NE(sh, nullptr) << "Expected fallback to StringHeader when custom parse fails";
  EXPECT_EQ(sh->to_string(), "some-value");
}

TEST_F(HeaderFactoryTest, RegistryRetention) {
  HeaderTestAccessor::publicRegistry().clear();

  EXPECT_TRUE(HeaderTestAccessor::publicRegistry().empty());

  Header::register_factory("X-One", []() -> std::shared_ptr<Header> {
    return std::make_shared<StringHeader>();
  });
  Header::register_factory("X-Two", []() -> std::shared_ptr<Header> {
    return std::make_shared<DummyHeader>();
  });

  EXPECT_EQ(HeaderTestAccessor::publicRegistry().size(), 2);
}
