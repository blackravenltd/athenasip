//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/events_api.h"

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "../helpers/signed_in_users_helper.h"
#include "../helpers/sync_datastore_helper.h"
#include "../mocks/logger_mock.h"
#include "api/admin_api.h"
#include "api/router.h"
#include "datastores/memory_datastore.h"
#include "events/local_event_system.h"
#include "types/url.h"

using namespace athenasip;

namespace {

// The local bus answers on whatever executor it is given; this one runs on its own thread for the connect.
// For the life of the process and never destroyed: its thread is detached, and an io_context destroyed at exit
// would be destroyed under it.
plugins::Executor admin_executor() {
  static auto* io = new boost::asio::io_context;
  static auto* guard = new boost::asio::executor_work_guard<boost::asio::io_context::executor_type>(io->get_executor());
  static bool started = (std::thread([]() { io->run(); }).detach(), true);
  (void)guard;
  (void)started;
  return io->get_executor();
}

struct EventsFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<datastores::MemoryDatastore> datastore;
  std::shared_ptr<SyncDatastore> store;
  std::shared_ptr<events::LocalEventSystem> bus;
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  std::shared_ptr<api::Sessions> sessions;
  std::shared_ptr<api::EventsAPI> events_api;
  SignedInUsers signed_in;

  explicit EventsFixture(std::size_t limit = 32) {
    datastore = std::make_shared<datastores::MemoryDatastore>(logger, std::make_shared<types::URL>("memory://"));
    store = std::make_shared<SyncDatastore>(datastore);
    store->connect();
    bus = std::make_shared<events::LocalEventSystem>(logger);
    std::promise<void> connected;
    bus->connect(admin_executor(), [&connected](plugins::Status) { connected.set_value(); });
    connected.get_future().wait();

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);
    auto bearer = std::make_shared<api::BearerAuth>();
    sessions = std::make_shared<api::Sessions>(logger, datastore, admin->executor(), api::Sessions::Lifetimes{3600, 600});
    bearer->sessions_register(sessions);
    router = std::make_shared<api::Router>(bearer);

    events_api = std::make_shared<api::EventsAPI>(logger, bus, admin->executor(), limit);
    events_api->register_routes(*router);
    admin->middlewares.push_back(router->middleware("/api/"));
    admin->streams_register(router->streams());
    admin->start();
    signed_in = SignedInUsers(sessions, *store);
  }

  ~EventsFixture() { admin->stop(); }

  // Opens the stream and returns the socket with the response read up to the end of its headers.
  struct Open {
    std::shared_ptr<boost::asio::io_context> io = std::make_shared<boost::asio::io_context>();
    std::shared_ptr<boost::asio::ip::tcp::socket> socket = std::make_shared<boost::asio::ip::tcp::socket>(*io);
    std::string received;
  };

  Open open(const std::string& token = "client-token") {
    Open stream;
    stream.socket->connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), admin->port()));

    std::string request = "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!token.empty()) request += "Authorization: Bearer " + signed_in.presented(token) + "\r\n";
    request += "\r\n";
    boost::asio::write(*stream.socket, boost::asio::buffer(request));

    read_until(stream, "\r\n\r\n");
    return stream;
  }

  // Reads until `text` has arrived, or two seconds pass.
  static bool read_until(Open& stream, const std::string& text) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    stream.socket->non_blocking(true);
    while (stream.received.find(text) == std::string::npos && std::chrono::steady_clock::now() < until) {
      char buffer[1024];
      boost::system::error_code ec;
      const auto size = stream.socket->read_some(boost::asio::buffer(buffer), ec);
      if (!ec) {
        stream.received.append(buffer, size);
      } else if (ec == boost::asio::error::would_block) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      } else {
        break;
      }
    }
    return stream.received.find(text) != std::string::npos;
  }

  bool wait_for_open(std::size_t count) {
    for (int i = 0; i < 400 && events_api->open() != count; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return events_api->open() == count;
  }
};

}  // namespace

// The text/event-stream format (HTML Living Standard, server-sent events): the event's name, a data line per
// line of the message, and a blank line.
TEST(EventsApiTest, AnEventIsItsNameAndADataLinePerLine) {
  EXPECT_EQ(api::EventsAPI::format("calls/abc/state", "{\"state\":\"answered\"}"), "event: calls/abc/state\ndata: {\"state\":\"answered\"}\n\n");
  EXPECT_EQ(api::EventsAPI::format("nodes/a/status", "one\r\ntwo"), "event: nodes/a/status\ndata: one\ndata: two\n\n");
}

// The stream answers 200 as text/event-stream and stays open, carrying what the bus publishes.
TEST(EventsApiTest, WhatTheBusPublishesArrivesOnTheStream) {
  EventsFixture f;

  auto stream = f.open();
  EXPECT_NE(stream.received.find("200"), std::string::npos) << stream.received;
  EXPECT_NE(stream.received.find("text/event-stream"), std::string::npos) << stream.received;
  ASSERT_TRUE(f.wait_for_open(1));

  // Subscriptions are made once the headers are out; give them a moment.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  f.bus->publish("calls/abc/state", "{\"state\":\"answered\"}");

  EXPECT_TRUE(EventsFixture::read_until(stream, "event: calls/abc/state\ndata: {\"state\":\"answered\"}\n\n")) << stream.received;
}

// Topics outside what a console watches are not sent.
TEST(EventsApiTest, OtherTopicsAreNotSent) {
  EventsFixture f;
  auto stream = f.open();
  ASSERT_TRUE(f.wait_for_open(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  f.bus->publish("something/else", "x");
  f.bus->publish("nodes/b/status", "{}");

  ASSERT_TRUE(EventsFixture::read_until(stream, "event: nodes/b/status"));
  EXPECT_EQ(stream.received.find("something/else"), std::string::npos);
}

// It needs the role the other status routes need.
TEST(EventsApiTest, NoTokenIsRefused) {
  EventsFixture f;

  auto stream = f.open("");

  EXPECT_NE(stream.received.find("401"), std::string::npos) << stream.received;
}

// A client going lets its stream go, and its subscriptions with it.
TEST(EventsApiTest, AClientGoingEndsItsStream) {
  EventsFixture f;
  {
    auto stream = f.open();
    ASSERT_TRUE(f.wait_for_open(1));
    stream.socket->close();
  }

  EXPECT_TRUE(f.wait_for_open(0));
}

// Each stream holds a connection; past the limit the node says it is busy.
TEST(EventsApiTest, PastTheLimitIs503) {
  EventsFixture f(1);
  auto first = f.open();
  ASSERT_TRUE(f.wait_for_open(1));

  auto second = f.open();

  EXPECT_NE(second.received.find("503"), std::string::npos) << second.received;
}
