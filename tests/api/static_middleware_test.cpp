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

// A document root with a secret next to it rather than inside it, which is the shape
// every path traversal is trying to reach. The sibling is deliberately named so that
// its path begins with the root's: a containment check written as a string comparison
// says that "/tmp/x/public-secrets" is inside "/tmp/x/public".
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

    // Somewhere outside the tree entirely, for the symlink.
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

  middleware(request, response, [&served, response](bool next) {
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

// A browser refuses a module script that does not arrive as JavaScript, and an SVG that
// does not arrive as image/svg+xml is not rendered as one. A built bundle is made of
// exactly these, so a document root that cannot name them cannot serve a web client -
// which is what this one exists to do.
TEST(StaticMiddlewareTest, NamesTheTypesABuiltBundleIsMadeOf) {
  DocumentRoot tree;

  EXPECT_EQ(get("/app/bundle.mjs", tree).content_type, "application/javascript");
  EXPECT_EQ(get("/app/logo.svg", tree).content_type, "image/svg+xml");
  EXPECT_EQ(get("/app/font.woff2", tree).content_type, "font/woff2");
  EXPECT_EQ(get("/favicon.ico", tree).content_type, "image/x-icon");

  // A source map is JSON, and a browser only fetches it when the developer tools ask.
  EXPECT_EQ(get("/app/bundle.js.map", tree).content_type, "application/json");
}

TEST(StaticMiddlewareTest, ServesTheIndexForADirectory) {
  DocumentRoot tree;

  EXPECT_EQ(get("/", tree).body, "<html>root</html>");
  EXPECT_EQ(get("/app/", tree).body, "<html>app</html>");
}

// A client-side router keeps real paths, and a person who reloads one, or opens a link
// to one, asks this node for a document that is not on disk. Answering 404 makes every
// page of the client work only if you arrived at it from somewhere else, which is not a
// working client. The document that knows how to route the path is index.html.
TEST(StaticMiddlewareTest, APathWithNoFileBehindItGetsTheDocumentThatCanRouteIt) {
  DocumentRoot tree;

  EXPECT_EQ(get("/diagnostics/softphone", tree).body, "<html>root</html>");
  EXPECT_EQ(get("/sip/realms", tree).body, "<html>root</html>");

  // The query is not part of the path here either.
  EXPECT_EQ(get("/diagnostics/softphone?register=1", tree).body, "<html>root</html>");

  // Its own type, not the type of whatever was asked for.
  EXPECT_EQ(get("/diagnostics/softphone", tree).content_type, "text/html");
}

// The fallback is for a route, and these are not routes. A missing asset has to stay
// missing: a bundle that 200s with HTML in it is a far worse thing to debug than one
// that 404s, and an unknown API path answering with a web page would be a lie about
// what this node serves.
TEST(StaticMiddlewareTest, TheFallbackDoesNotInventAssetsOrApiRoutes) {
  DocumentRoot tree;

  EXPECT_TRUE(get("/app/missing.js", tree).passed_on);
  EXPECT_TRUE(get("/app/missing.css", tree).passed_on);
  EXPECT_TRUE(get("/api/v1/nothing", tree).passed_on);

  // A write to a path that does not exist is not a page view.
  EXPECT_TRUE(serve(boost::beast::http::verb::post, "/diagnostics/softphone", tree).passed_on);
  EXPECT_TRUE(serve(boost::beast::http::verb::delete_, "/sip/realms", tree).passed_on);
}

// And a file that is really there is still served as itself.
TEST(StaticMiddlewareTest, TheFallbackNeverShadowsAFileThatExists) {
  DocumentRoot tree;

  EXPECT_EQ(get("/app/bundle.js", tree).content_type, "application/javascript");
  EXPECT_EQ(get("/app/", tree).body, "<html>app</html>");
}

TEST(StaticMiddlewareTest, PassesOnWhatItDoesNotHave) {
  DocumentRoot tree;

  EXPECT_TRUE(get("/nothing/here.js", tree).passed_on);
}

// A document root is a boundary. Every one of these is a way of asking for something on
// the other side of it, and the answer to all of them is the same.
TEST(StaticMiddlewareTest, RefusesToLeaveTheDocumentRoot) {
  DocumentRoot tree;

  EXPECT_TRUE(get("/../public-secrets/secret.txt", tree).passed_on);
  EXPECT_TRUE(get("/app/../../public-secrets/secret.txt", tree).passed_on);
  EXPECT_TRUE(get("/./../public-secrets/secret.txt", tree).passed_on);

  // Percent-encoded, because a check that looks for the two characters and not for what
  // they mean is a check that is one decoder away from being no check at all.
  EXPECT_TRUE(get("/%2e%2e/public-secrets/secret.txt", tree).passed_on);
  EXPECT_TRUE(get("/%2E%2E/public-secrets/secret.txt", tree).passed_on);

  // An absolute path, in case the root is treated as a prefix to concatenate rather
  // than a directory to resolve within. The routing fallback does not stand in for it
  // either: a route is made of names, and this one has an empty segment.
  EXPECT_TRUE(get("//etc/hosts", tree).passed_on);
  EXPECT_TRUE(get("//etc/shadow", tree).passed_on);
}

// A symlink is the other way out, and the one the path check cannot see: the path never
// mentions anywhere it should not go. Only resolving the file and asking where it
// actually is catches this.
TEST(StaticMiddlewareTest, RefusesASymlinkThatPointsOutOfTheDocumentRoot) {
  DocumentRoot tree;

  if (!fs::is_symlink(tree.root / "escape.txt")) GTEST_SKIP() << "this filesystem would not make a symlink";

  EXPECT_TRUE(get("/escape.txt", tree).passed_on);
}

// The sibling directory exists to catch a containment check written as a string
// comparison: "/tmp/x/public-secrets/secret.txt" starts with "/tmp/x/public", and is
// not inside it. Reached through a symlink, because the path itself is unremarkable.
TEST(StaticMiddlewareTest, ADirectoryWhoseNameExtendsTheRootIsNotInsideIt) {
  DocumentRoot tree;

  std::error_code ec;
  fs::create_symlink(tree.sibling / "secret.txt", tree.root / "sibling.txt", ec);

  if (!fs::is_symlink(tree.root / "sibling.txt")) GTEST_SKIP() << "this filesystem would not make a symlink";

  EXPECT_TRUE(get("/sibling.txt", tree).passed_on);
}

// A query string is not part of the path. Every web application cache-busts with one,
// and the admin client is a built bundle that does exactly this, so a middleware that
// looks for a file called "bundle.js?v=2" serves a blank page to anybody who deploys a
// second version.
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

// A fragment never reaches a server from a browser, but nothing stops a client sending
// one, and it is part of the URI rather than of the path either way (RFC 3986 3.5).
TEST(StaticMiddlewareTest, AFragmentIsNotPartOfTheFilenameEither) {
  DocumentRoot tree;

  EXPECT_EQ(get("/app/bundle.js#top", tree).body, "console.log(1)");
}

// The prefix is what says which requests are the middleware's, so anything outside it
// belongs to the API routes that come after.
TEST(StaticMiddlewareTest, RequestsOutsideThePrefixArePassedOn) {
  DocumentRoot tree;

  StaticOptions options;
  options.prefix = "/ui/";

  EXPECT_TRUE(get("/api/v1/realms", tree, options).passed_on);
  EXPECT_EQ(get("/ui/app/bundle.js", tree, options).body, "console.log(1)");
}
