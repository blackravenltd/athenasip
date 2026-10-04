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

// An engine that anchors nothing and reports whatever idleness the test sets. The builtin relay
// stamps activity from the real clock, so the sweep is tested against this stub instead.
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

  // SDP passes through untouched.
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

  // Alice calls Bob and Bob answers: a confirmed dialog.
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

// A call whose media has been idle for sip_media_timeout is torn down and its relay released: an
// endpoint that vanishes sends no BYE, and relay ports are a finite pool.
TEST(CoreMediaSweepTest, ACallWhoseMediaHasStoppedIsLetGo) {
  SweepFixture f;
  f.config->sip_media_timeout = 300;

  f.connect_call();
  ASSERT_EQ(f.dialogs().size(), 1u);

  // Media still flowing: the call stays however long it runs.
  f.engine->idle_seconds = 0;
  f.advance(std::chrono::seconds(3600));
  EXPECT_EQ(f.dialogs().size(), 1u);

  // Idle, but for less than the timeout.
  f.engine->idle_seconds = 299;
  f.advance(std::chrono::seconds(120));
  EXPECT_EQ(f.dialogs().size(), 1u);

  f.engine->idle_seconds = 300;
  f.advance(std::chrono::seconds(120));

  EXPECT_EQ(f.dialogs().size(), 0u);
  EXPECT_GT(f.engine->releases.load(), 0);
}

// RFC 4028 section 8.3: a proxy may drop call state and free resources but MUST NOT send a BYE.
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

// sip_media_timeout: 0 turns the media timeout off.
TEST(CoreMediaSweepTest, ATimeoutOfZeroNeverEndsACall) {
  SweepFixture f;
  f.config->sip_media_timeout = 0;

  f.connect_call();
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.engine->idle_seconds = 100000;
  f.advance(std::chrono::hours(2));

  EXPECT_EQ(f.dialogs().size(), 1u);
}

// A call the engine holds no media for (media end to end) reports no idleness and is left alone.
TEST(CoreMediaSweepTest, ACallTheEngineHoldsNothingForIsLeftAlone) {
  SweepFixture f;
  f.config->sip_media_timeout = 60;

  f.connect_call();
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.engine->holds_media = false;
  f.advance(std::chrono::seconds(600));

  EXPECT_EQ(f.dialogs().size(), 1u);
}

// sip_max_call_duration is the backstop for calls with no anchored media and no session timer
// (RFC 4028). It cuts legitimate long calls too, so it is off by default.
TEST(CoreMediaSweepTest, ACallLongerThanTheMaximumIsLetGo) {
  SweepFixture f;
  f.config->sip_media_timeout = 0;
  f.config->sip_max_call_duration = 3600;

  f.connect_call();
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.advance(std::chrono::seconds(3000));
  EXPECT_EQ(f.dialogs().size(), 1u);

  f.advance(std::chrono::seconds(1200));
  EXPECT_EQ(f.dialogs().size(), 0u);
}

// The maximum applies to a call this node anchors no media for.
TEST(CoreMediaSweepTest, TheMaximumReachesACallWithNoMediaToWatch) {
  SweepFixture f;
  f.config->sip_media_timeout = 300;
  f.config->sip_max_call_duration = 1800;

  f.connect_call();
  f.engine->holds_media = false;
  ASSERT_EQ(f.dialogs().size(), 1u);

  // The media timeout does not apply: there is no media to watch.
  f.advance(std::chrono::seconds(900));
  EXPECT_EQ(f.dialogs().size(), 1u);

  f.advance(std::chrono::seconds(1200));
  EXPECT_EQ(f.dialogs().size(), 0u);
}

// sip_max_call_duration: 0 turns the cap off.
TEST(CoreMediaSweepTest, AMaximumOfZeroNeverEndsACall) {
  SweepFixture f;
  f.config->sip_media_timeout = 300;
  f.config->sip_max_call_duration = 0;

  f.connect_call();
  f.engine->idle_seconds = 0;
  ASSERT_EQ(f.dialogs().size(), 1u);

  f.advance(std::chrono::hours(12));

  EXPECT_EQ(f.dialogs().size(), 1u);
}

// RFC 4028 section 8.3 holds for the duration cap too: no BYE to either end.
TEST(CoreMediaSweepTest, TheMaximumSendsNoByeEither) {
  SweepFixture f;
  f.config->sip_media_timeout = 0;
  f.config->sip_max_call_duration = 60;

  f.connect_call();
  f.advance(std::chrono::seconds(120));

  ASSERT_EQ(f.dialogs().size(), 0u);

  EXPECT_TRUE(ProxyFixture::requests_with(f.caller_connection, "BYE").empty());
  EXPECT_TRUE(ProxyFixture::requests_with(f.callee_connection, "BYE").empty());
}
