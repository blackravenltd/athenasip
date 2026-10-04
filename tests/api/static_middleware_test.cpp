//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/static_middleware.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace athenasip::api;

namespace fs = std::filesystem;

namespace {

// A document root with a secret beside it. The sibling's path begins with the root's, to catch a containment
// check written as a string comparison.
struct DocumentRoot {
  fs::path base;
  fs::path root;
  fs::path sibling;

  DocumentRoot() {
    base = fs::temp_directory_path() / fs::path("athena-static-" + std::to_string(::getpid()) + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));

    root = base / "public";
    sibling = base / "public-secrets";

    fs::create_directories(root / "app");
    fs::create_directories(sibling);

    write(root / "index.html", "<html>root</html>");
    write(root / "app" / "index.html", "<html>app</html>");
    write(root / "app" / "bundle.js", "console.log(1)");
    write(root / "app" / "bundle.mjs", "export default 1");
    write(root / "app" / "bundle.js.map", "{}");
    write(root / "app" / "logo.svg", "<svg/>");
    write(root / "app" / "font.woff2", "woff");
    write(root / "favicon.ico", "icon");
    write(sibling / "secret.txt", "the secret");

    // Outside the tree entirely, for the symlink.
    write(base / "outside.txt", "outside");
    std::error_code ec;
    fs::create_symlink(base / "outside.txt", root / "escape.txt", ec);
  }

  ~DocumentRoot() {
    std::error_code ec;
    fs::remove_all(base, ec);
  }

  static void write(const fs::path& at, const std::string& text) {
    std::ofstream file(at);
    file << text;
  }
};

// What the middleware did with one request: whether it served something, and what.
struct Served {
  bool passed_on = true;
  std::string body;
  std::string content_type;
};

Served serve(boost::beast::http::verb method, const std::string& target, const DocumentRoot& tree, const StaticOptions& options = StaticOptions()) {
  auto middleware = StaticMiddleware::add(tree.root.string(), options);

  boost::beast::http::request<boost::beast::http::string_body> request;
  request.method(method);
  request.target(target);

  auto response = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>();

  Served served;

  middleware(request, "127.0.0.1", response, [&served, response](bool next) {
    served.passed_on = next;

    if (!next) {
      served.body = response->body();
      served.content_type = std::string(response->at(boost::beast::http::field::content_type));
    }
  });

  return served;
}

Served get(const std::string& target, const DocumentRoot& tree, const StaticOptions& options = StaticOptions()) {
  return serve(boost::beast::http::verb::get, target, tree, options);
}

}  // namespace

TEST(StaticMiddlewareTest, ServesAFileFromTheDocumentRootWithItsType) {
  DocumentRoot tree;

  const auto served = get("/app/bundle.js", tree);

  EXPECT_FALSE(served.passed_on);
  EXPECT_EQ(served.body, "console.log(1)");
  EXPECT_EQ(served.content_type, "application/javascript");
}

// A browser refuses a module script not served as JavaScript and an SVG not served as image/svg+xml.
TEST(StaticMiddlewareTest, NamesTheTypesABuiltBundleIsMadeOf) {
  DocumentRoot tree;

  EXPECT_EQ(get("/app/bundle.mjs", tree).content_type, "application/javascript");
  EXPECT_EQ(get("/app/logo.svg", tree).content_type, "image/svg+xml");
  EXPECT_EQ(get("/app/font.woff2", tree).content_type, "font/woff2");
  EXPECT_EQ(get("/favicon.ico", tree).content_type, "image/x-icon");

  // A source map is JSON.
  EXPECT_EQ(get("/app/bundle.js.map", tree).content_type, "application/json");
}

TEST(StaticMiddlewareTest, ServesTheIndexForADirectory) {
  DocumentRoot tree;

  EXPECT_EQ(get("/", tree).body, "<html>root</html>");
  EXPECT_EQ(get("/app/", tree).body, "<html>app</html>");
}

// A client-side router keeps real paths, so a path with no file behind it gets index.html, which can route it.
TEST(StaticMiddlewareTest, APathWithNoFileBehindItGetsTheDocumentThatCanRouteIt) {
  DocumentRoot tree;

  EXPECT_EQ(get("/diagnostics/softphone", tree).body, "<html>root</html>");
  EXPECT_EQ(get("/sip/realms", tree).body, "<html>root</html>");

  // The query is not part of the path.
  EXPECT_EQ(get("/diagnostics/softphone?register=1", tree).body, "<html>root</html>");

  // With index.html's own type, not that of the path asked for.
  EXPECT_EQ(get("/diagnostics/softphone", tree).content_type, "text/html");
}

// The fallback is for routes only: a missing asset and an unknown API path stay missing.
TEST(StaticMiddlewareTest, TheFallbackDoesNotInventAssetsOrApiRoutes) {
  DocumentRoot tree;

  EXPECT_TRUE(get("/app/missing.js", tree).passed_on);
  EXPECT_TRUE(get("/app/missing.css", tree).passed_on);
  EXPECT_TRUE(get("/api/v1/nothing", tree).passed_on);

  // A write to a path that does not exist is not a page view.
  EXPECT_TRUE(serve(boost::beast::http::verb::post, "/diagnostics/softphone", tree).passed_on);
  EXPECT_TRUE(serve(boost::beast::http::verb::delete_, "/sip/realms", tree).passed_on);
}

// A file that exists is served as itself.
TEST(StaticMiddlewareTest, TheFallbackNeverShadowsAFileThatExists) {
  DocumentRoot tree;

  EXPECT_EQ(get("/app/bundle.js", tree).content_type, "application/javascript");
  EXPECT_EQ(get("/app/", tree).body, "<html>app</html>");
}

// The fallback is opt-in: a plain file server answers 404 for a missing page.
TEST(StaticMiddlewareTest, TheRoutingFallbackCanBeTurnedOff) {
  DocumentRoot tree;

  StaticOptions plain;
  plain.fallback.clear();

  EXPECT_TRUE(get("/diagnostics/softphone", tree, plain).passed_on);
  EXPECT_TRUE(get("/sip/realms", tree, plain).passed_on);

  // What exists is still served.
  EXPECT_EQ(get("/app/bundle.js", tree, plain).content_type, "application/javascript");
  EXPECT_EQ(get("/", tree, plain).body, "<html>root</html>");
}

TEST(StaticMiddlewareTest, PassesOnWhatItDoesNotHave) {
  DocumentRoot tree;

  EXPECT_TRUE(get("/nothing/here.js", tree).passed_on);
}

// Every one of these asks for something outside the document root, and all are refused.
TEST(StaticMiddlewareTest, RefusesToLeaveTheDocumentRoot) {
  DocumentRoot tree;

  EXPECT_TRUE(get("/../public-secrets/secret.txt", tree).passed_on);
  EXPECT_TRUE(get("/app/../../public-secrets/secret.txt", tree).passed_on);
  EXPECT_TRUE(get("/./../public-secrets/secret.txt", tree).passed_on);

  // Percent-encoded dots are decoded before the check.
  EXPECT_TRUE(get("/%2e%2e/public-secrets/secret.txt", tree).passed_on);
  EXPECT_TRUE(get("/%2E%2E/public-secrets/secret.txt", tree).passed_on);

  // An absolute path, in case the root is treated as a prefix to concatenate. Its empty segment also keeps the
  // routing fallback from answering.
  EXPECT_TRUE(get("//etc/hosts", tree).passed_on);
  EXPECT_TRUE(get("//etc/shadow", tree).passed_on);
}

// A symlink leaves the root without the path saying so; only resolving the file catches it.
TEST(StaticMiddlewareTest, RefusesASymlinkThatPointsOutOfTheDocumentRoot) {
  DocumentRoot tree;

  if (!fs::is_symlink(tree.root / "escape.txt")) GTEST_SKIP() << "this filesystem would not make a symlink";

  EXPECT_TRUE(get("/escape.txt", tree).passed_on);
}

// "/tmp/x/public-secrets/secret.txt" starts with "/tmp/x/public" and is not inside it. Reached through a symlink.
TEST(StaticMiddlewareTest, ADirectoryWhoseNameExtendsTheRootIsNotInsideIt) {
  DocumentRoot tree;

  std::error_code ec;
  fs::create_symlink(tree.sibling / "secret.txt", tree.root / "sibling.txt", ec);

  if (!fs::is_symlink(tree.root / "sibling.txt")) GTEST_SKIP() << "this filesystem would not make a symlink";

  EXPECT_TRUE(get("/sibling.txt", tree).passed_on);
}

// A query string is not part of the path: built bundles cache-bust with one.
TEST(StaticMiddlewareTest, AQueryStringIsNotPartOfTheFilename) {
  DocumentRoot tree;

  const auto served = get("/app/bundle.js?v=2", tree);

  EXPECT_FALSE(served.passed_on);
  EXPECT_EQ(served.body, "console.log(1)");
}

TEST(StaticMiddlewareTest, AQueryStringOnADirectoryStillFindsTheIndex) {
  DocumentRoot tree;

  EXPECT_EQ(get("/?first=true", tree).body, "<html>root</html>");
}

// RFC 3986 3.5: a fragment is not part of the path either.
TEST(StaticMiddlewareTest, AFragmentIsNotPartOfTheFilenameEither) {
  DocumentRoot tree;

  EXPECT_EQ(get("/app/bundle.js#top", tree).body, "console.log(1)");
}

// Anything outside the prefix belongs to the API routes that come after.
TEST(StaticMiddlewareTest, RequestsOutsideThePrefixArePassedOn) {
  DocumentRoot tree;

  StaticOptions options;
  options.prefix = "/ui/";

  EXPECT_TRUE(get("/api/v1/realms", tree, options).passed_on);
  EXPECT_EQ(get("/ui/app/bundle.js", tree, options).body, "console.log(1)");
}
