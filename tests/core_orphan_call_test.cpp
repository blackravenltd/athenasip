//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <string>

#include "helpers/core_fixture_helper.h"
#include "media/media_engine.h"

using namespace athenasip;
using athenasip::media::Capabilities;
using athenasip::media::Flags;
using athenasip::media::MediaEngine;
using athenasip::media::Result;

namespace {

// Reports whatever the test sets for any call it is asked about.
class StubMediaEngine : public MediaEngine {
 public:
  std::atomic<std::int64_t> idle_seconds{0};
  std::atomic<bool> held{true};
  std::atomic<bool> answers{true};
  std::atomic<int> releases{0};
  std::atomic<int> queries{0};

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
    queries++;
    if (!answers.load()) return _complete(std::move(on), std::move(handler), plugins::Result<std::string>::failure("no answer"));

    const std::string document = held.load() ? "{\"call_id\":\"" + call->id + "\",\"idle_seconds\":" + std::to_string(idle_seconds.load()) + "}"
                                             : "{\"call_id\":\"" + call->id + "\",\"held\":false,\"idle_seconds\":null}";
    _complete(std::move(on), std::move(handler), plugins::Result<std::string>::success(document));
  }

 private:
  bool _connected = false;
};

struct OrphanFixture : CoreFixture {
  std::shared_ptr<StubMediaEngine> engine = std::make_shared<StubMediaEngine>();

  OrphanFixture() {
    config->sip_media_timeout = 300;
    config->events_status_interval = 30;

    on_strand([this]() { engine->connect(core->strand(), nullptr); });
    settle();
    core->media_register(engine);
  }

  // A call in the store, up through another node.
  std::shared_ptr<Call> stored_call(const std::string& id, const std::string& node, const std::string& media_engine = "stub 0.0.1") {
    auto call = std::make_shared<Call>();
    call->id = id;
    call->node = node;
    call->state = Call::State::Connected;
    call->media_engine = media_engine;
    call->created_at = std::time(nullptr) - 600;
    call->answered_at = call->created_at + 5;
    EXPECT_TRUE(store->call_create(call));
    return call;
  }

  // A node the bus has heard from.
  void heard(const std::string& node, const std::string& status = "ok") {
    core->nodes()->observe("nodes/" + node + "/status", "{\"status\":\"" + status + "\",\"node\":\"" + node + "\"}");
  }

  Call::State state_of(const std::string& id) { return store->call_get(id)->state; }

  void advance(std::chrono::seconds by) {
    on_strand([this, by]() { timers->advance(std::chrono::duration_cast<std::chrono::milliseconds>(by)); });
    settle();
  }

  // Past the three status intervals a node watches the cluster before it judges another gone, and the sweep after.
  void watch_the_cluster() { advance(std::chrono::seconds(180)); }
};

}  // namespace

// A dead node's anchored call goes on as long as its media does: rtpengine relays without the node. Once the media
// stops, the record is closed and the engine's ports released.
TEST(CoreOrphanCallTest, ACallOfANodeThatHasGoneIsClosedOnceItsMediaStops) {
  OrphanFixture f;
  f.stored_call("orphan-1", "node-gone");

  f.engine->idle_seconds = 0;
  f.watch_the_cluster();
  f.advance(std::chrono::seconds(600));
  EXPECT_EQ(f.state_of("orphan-1"), Call::State::Connected);
  EXPECT_EQ(f.engine->releases.load(), 0);

  f.engine->idle_seconds = 300;
  f.advance(std::chrono::seconds(120));

  auto call = f.store->call_get("orphan-1");
  EXPECT_EQ(call->state, Call::State::Closed);
  EXPECT_GT(call->ended_at, 0);
  EXPECT_GT(f.engine->releases.load(), 0);
}

// The engine has already let the call go (its own timeout): nothing to wait for.
TEST(CoreOrphanCallTest, ACallTheEngineNoLongerHoldsIsClosed) {
  OrphanFixture f;
  f.stored_call("orphan-2", "node-gone");

  f.engine->held = false;
  f.watch_the_cluster();

  EXPECT_EQ(f.state_of("orphan-2"), Call::State::Closed);
}

// Media end to end never touched the dead node, and no node can see it: the record is closed when the node is
// found gone, rather than left open for ever.
TEST(CoreOrphanCallTest, AnUnanchoredCallOfANodeThatHasGoneIsClosed) {
  OrphanFixture f;
  f.stored_call("orphan-3", "node-gone", "");

  f.watch_the_cluster();

  EXPECT_EQ(f.state_of("orphan-3"), Call::State::Closed);
  EXPECT_EQ(f.engine->queries.load(), 0);
}

// An engine that cannot be asked says nothing about the call, which is left as it is.
TEST(CoreOrphanCallTest, ACallTheEngineCannotBeAskedAboutIsLeftAlone) {
  OrphanFixture f;
  f.stored_call("orphan-4", "node-gone");

  f.engine->answers = false;
  f.watch_the_cluster();
  f.advance(std::chrono::seconds(600));

  EXPECT_EQ(f.state_of("orphan-4"), Call::State::Connected);
  EXPECT_EQ(f.engine->releases.load(), 0);
}

TEST(CoreOrphanCallTest, ACallOfALiveNodeIsLeftAlone) {
  OrphanFixture f;
  f.stored_call("live-1", "node-live");
  f.heard("node-live");

  f.engine->held = false;
  f.watch_the_cluster();
  f.heard("node-live");
  f.advance(std::chrono::seconds(120));

  EXPECT_EQ(f.state_of("live-1"), Call::State::Connected);
}

// A node that has just started has not heard the cluster yet, and every other node would look gone.
TEST(CoreOrphanCallTest, NothingIsClosedBeforeTheNodeHasWatchedTheClusterForThreeIntervals) {
  OrphanFixture f;
  f.stored_call("early-1", "node-quiet", "");

  // The first sweep, at 75 seconds, is inside the 90.
  f.advance(std::chrono::seconds(80));
  EXPECT_EQ(f.state_of("early-1"), Call::State::Connected);

  f.advance(std::chrono::seconds(80));
  EXPECT_EQ(f.state_of("early-1"), Call::State::Closed);
}

// One node does the closing, the live node with the lowest id, so a cluster asks the engine once per call.
TEST(CoreOrphanCallTest, OnlyTheLiveNodeWithTheLowestIdClosesAnything) {
  OrphanFixture f;
  f.stored_call("orphan-5", "node-gone", "");
  f.heard("a-node");

  f.watch_the_cluster();
  f.heard("a-node");
  f.advance(std::chrono::seconds(60));

  EXPECT_EQ(f.state_of("orphan-5"), Call::State::Connected);
}

// A node that said it was stopping, or whose broker said it went, is gone at once.
TEST(CoreOrphanCallTest, ANodeThatSaidItWasDownIsGone) {
  OrphanFixture f;
  f.stored_call("orphan-6", "node-down", "");
  f.heard("node-down", "down");

  f.watch_the_cluster();

  EXPECT_EQ(f.state_of("orphan-6"), Call::State::Closed);
}

// This node's own calls from before a restart: the process that held their dialogs is gone too.
TEST(CoreOrphanCallTest, ThisNodesCallsFromBeforeARestartAreClosed) {
  OrphanFixture f;
  f.stored_call("before-restart", f.config->sip_node_id, "");

  f.watch_the_cluster();

  EXPECT_EQ(f.state_of("before-restart"), Call::State::Closed);
}

// The closing is told to whoever watches calls, as any other call ending is.
TEST(CoreOrphanCallTest, ClosingACallIsPublished) {
  OrphanFixture f;
  f.stored_call("orphan-7", "node-gone", "");

  std::vector<std::string> states;
  f.bus->subscribe("calls/orphan-7/state", [&states](const std::string&, const std::string& message) { states.push_back(message); });

  f.watch_the_cluster();

  ASSERT_FALSE(states.empty());
  EXPECT_EQ(states.back(), "Closed");
}
