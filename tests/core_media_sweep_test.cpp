//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "helpers/proxy_fixture_helper.h"
#include "media/media_engine.h"

using namespace athenasip;
using athenasip::media::Capabilities;
using athenasip::media::Flags;
using athenasip::media::MediaEngine;
using athenasip::media::Result;

namespace {

// An engine that anchors nothing and reports whatever idleness the test sets.
//
// The relay's own clock is the real one, because it is stamped in the receive handler on
// the packet path and a manual clock has no business there. That leaves the sweep's own
// behaviour - read the engine's answer, decide, tear the call down - needing a way to be
// asked a question whose answer the test chooses. The relay's half, that a packet
// arriving resets the reading, is a builtin engine test.
class StubMediaEngine : public MediaEngine {
 public:
  std::atomic<std::int64_t> idle_seconds{0};
  std::atomic<bool> holds_media{true};
  std::atomic<int> releases{0};

  std::string name() const override { return "stub"; }
  std::string version() const override { return "0.0.1"; }

  void connect(plugins::Executor on, plugins::StatusHandler handler) override {
    _connected = true;
    _complete(std::move(on), std::move(handler), plugins::Status::success());
  }

  void close() override { _connected = false; }
  bool is_connected() const override { return _connected; }

  Capabilities capabilities() const override {
    Capabilities capabilities;
    capabilities.bridge = true;
    return capabilities;
  }

  // The description travels on untouched. This engine is here for what it says about a
  // call, not for what it does to one.
  void offer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) override {
    (void)call;
    (void)flags;
    _complete(std::move(on), std::move(handler), Result::success(std::move(sdp)));
  }

  void answer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) override {
    (void)call;
    (void)flags;
    _complete(std::move(on), std::move(handler), Result::success(std::move(sdp)));
  }

  void release(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override {
    (void)call;
    releases++;
    _complete(std::move(on), std::move(handler), plugins::Status::success());
  }

  void query(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::string> handler) override {
    const std::string idle = holds_media.load() ? std::to_string(idle_seconds.load()) : std::string("null");
    const std::string document = "{\"call_id\":\"" + (call ? call->id : std::string()) + "\",\"engine\":\"stub\",\"idle_seconds\":" + idle + "}";

    _complete(std::move(on), std::move(handler), plugins::Result<std::string>::success(document));
  }

 private:
  bool _connected = false;
};

struct SweepFixture : ProxyFixture {
  std::shared_ptr<StubMediaEngine> engine = std::make_shared<StubMediaEngine>();

  SweepFixture() {
    on_strand([this]() { engine->connect(core->strand(), nullptr); });
    settle();

    core->media_register(engine);
    bind_bob();
  }

  // Alice calls Bob and Bob answers, which is what makes a confirmed dialog.
  void connect_call() {
    receive(caller, invite());
    receive(callee, response_from_callee(200, "OK"));
    settle();
  }

  void advance(std::chrono::seconds by) {
    on_strand([this, by]() { timers->advance(std::chrono::duration_cast<std::chrono::milliseconds>(by)); });
    settle();
  }
};

}  // namespace

// A phone that loses power sends no BYE, and a call between two endpoints that never
// negotiated a session timer has no expiry of its own. Nothing in the signalling plane
// will ever say that call ended, so the node held its dialog, its call record and its
// relay ports until the process restarted - and the ports are a finite pool.
TEST(CoreMediaSweepTest, ACallWhoseMediaHasStoppedIsLetGo) {
  SweepFixture f;
  f.config->sip_media_timeout = 300;

  f.connect_call();
  ASSERT_EQ(f.dialogs().size(), 1u);

  // Media still flowing. However long the call runs, it is not this node's to end.
  f.engine->idle_seconds = 0;
  f.advance(std::chrono::seconds(3600));
  EXPECT_EQ(f.dialogs().size(), 1u);

  // Quiet, but not for long enough yet.
  f.engine->idle_seconds = 299;
  f.advance(std::chrono::seconds(120));
  EXPECT_EQ(f.dialogs().size(), 1u);

  f.engine->idle_seconds = 300;
  f.advance(std::chrono::seconds(120));

  EXPECT_EQ(f.dialogs().size(), 0u);
  EXPECT_GT(f.engine->releases.load(), 0);
}

// RFC 4028 section 8.3: "the proxy MAY remove associated call state, and MAY free any
// resources associated with the call. Unlike the UA, it MUST NOT send a BYE." This node
// is on the path of the dialog, not an end of it, and a BYE from here would be it acting
// as a user agent in somebody else's call. Both ends run their own timers and will each
// send their own when they notice.
TEST(CoreMediaSweepTest, NoByeIsSentToEitherEnd) {
  SweepFixture f;
  f.config->sip_media_timeout = 60;

  f.connect_call();
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.engine->idle_seconds = 600;
  f.advance(std::chrono::seconds(120));

  ASSERT_EQ(f.dialogs().size(), 0u);

  EXPECT_TRUE(ProxyFixture::requests_with(f.caller_connection, "BYE").empty());
  EXPECT_TRUE(ProxyFixture::requests_with(f.callee_connection, "BYE").empty());
}

// Zero is off, and off has to mean the node never ends a call on its own account.
TEST(CoreMediaSweepTest, ATimeoutOfZeroNeverEndsACall) {
  SweepFixture f;
  f.config->sip_media_timeout = 0;

  f.connect_call();
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.engine->idle_seconds = 100000;
  f.advance(std::chrono::hours(2));

  EXPECT_EQ(f.dialogs().size(), 1u);
}

// An engine holding nothing for a call is not an engine reporting a very idle one. Where
// the media went end to end - the engine declined the description, or a realm passes it
// through - there is nothing to watch and nothing to conclude.
TEST(CoreMediaSweepTest, ACallTheEngineHoldsNothingForIsLeftAlone) {
  SweepFixture f;
  f.config->sip_media_timeout = 60;

  f.connect_call();
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.engine->holds_media = false;
  f.advance(std::chrono::seconds(600));

  EXPECT_EQ(f.dialogs().size(), 1u);
}
