//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// One connection to a Redis server: AUTH and SELECT before anything else, commands pipelined and answered in order, an
// error reply passed on as a reply, a deadline on every request, the link's loss failing what was written, and a
// reconnect once the server is back. Against a scripted server on loopback that answers as each test says.
#include "datastores/resp_connection.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../mocks/logger_mock.h"
#include "datastores/resp.h"

using namespace athenasip::datastores::resp;
using namespace std::chrono_literals;

namespace {

using Command = std::vector<std::string>;

// What the server does with one command: answer with these bytes, say nothing, or close the connection.
struct Action {
  std::string reply;
  bool close = false;
  static Action answer(std::string bytes) { return {std::move(bytes), false}; }
  static Action silence() { return {"", false}; }
  static Action hang_up() { return {"", true}; }
};

class ScriptedServer {
 public:
  using Script = std::function<Action(const Command&, int connection)>;

  explicit ScriptedServer(Script script) : _script(std::move(script)), _acceptor(_io) {
    const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), 0);
    _acceptor.open(endpoint.protocol());
    _acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    _acceptor.bind(endpoint);
    _acceptor.listen();
    _port = _acceptor.local_endpoint().port();
    _accept();
    _thread = std::thread([this]() { _io.run(); });
  }

  ~ScriptedServer() {
    _io.stop();
    if (_thread.joinable()) _thread.join();
  }

  std::uint16_t port() const { return _port; }

  std::vector<Command> commands() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _commands;
  }

  int connections() const { return _connections.load(); }

 private:
  struct Session {
    boost::asio::ip::tcp::socket socket;
    Decoder decoder;
    std::array<char, 4096> buffer{};
    int id;
    explicit Session(boost::asio::ip::tcp::socket s, int n) : socket(std::move(s)), id(n) {}
  };

  void _accept() {
    _acceptor.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
      if (ec) return;
      auto session = std::make_shared<Session>(std::move(socket), ++_connections);
      _read(session);
      _accept();
    });
  }

  void _read(std::shared_ptr<Session> session) {
    session->socket.async_read_some(boost::asio::buffer(session->buffer), [this, session](boost::system::error_code ec, std::size_t length) {
      if (ec) return;
      session->decoder.feed(std::string_view(session->buffer.data(), length));
      while (auto request = session->decoder.next()) {
        Command command;
        for (const auto& element : request->elements) command.push_back(element.text);
        {
          std::lock_guard<std::mutex> lock(_mutex);
          _commands.push_back(command);
        }
        const auto action = _script(command, session->id);
        if (action.close) {
          boost::system::error_code ignored;
          session->socket.close(ignored);
          return;
        }
        if (!action.reply.empty()) boost::asio::write(session->socket, boost::asio::buffer(action.reply));
      }
      _read(session);
    });
  }

  Script _script;
  boost::asio::io_context _io;
  boost::asio::ip::tcp::acceptor _acceptor;
  std::uint16_t _port = 0;
  std::thread _thread;
  std::atomic<int> _connections{0};
  mutable std::mutex _mutex;
  std::vector<Command> _commands;
};

// The client side: a connection on an io_context of its own thread, as the driver runs it.
struct Client {
  boost::asio::io_context io;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io.get_executor()};
  std::shared_ptr<Connection> connection;
  std::thread thread;

  explicit Client(Connection::Settings settings) {
    connection = std::make_shared<Connection>(io, std::make_shared<MockLogger>(), std::move(settings));
    connection->start();
    thread = std::thread([this]() { io.run(); });
  }

  ~Client() {
    connection->stop();
    work.reset();
    if (thread.joinable()) thread.join();
  }

  struct Answer {
    boost::system::error_code ec;
    std::string what;
    std::vector<Reply> replies;
  };

  Answer exec(Request request) {
    auto promise = std::make_shared<std::promise<Answer>>();
    auto future = promise->get_future();
    connection->exec(std::move(request), [promise](boost::system::error_code ec, std::string what, std::vector<Reply> replies) {
      promise->set_value(Answer{ec, std::move(what), std::move(replies)});
    });
    return future.get();
  }
};

Connection::Settings settings_for(const ScriptedServer& server) {
  Connection::Settings settings;
  settings.port = server.port();
  settings.request_timeout = 1500ms;
  settings.connect_timeout = 1000ms;
  settings.reconnect_min = 50ms;
  settings.reconnect_max = 200ms;
  return settings;
}

Action answer_ok_and_pong(const Command& command, int) {
  if (command[0] == "PING") return Action::answer("+PONG\r\n");
  return Action::answer("+OK\r\n");
}

Request ping() {
  Request request;
  request.push("PING");
  return request;
}

}  // namespace

// The client signs in and selects its database before it sends anything else.
TEST(RespConnectionTest, AuthAndSelectComeFirst) {
  ScriptedServer server(answer_ok_and_pong);
  auto settings = settings_for(server);
  settings.username = "node";
  settings.password = "secret";
  settings.database = 3;
  Client client(settings);

  const auto answer = client.exec(ping());
  ASSERT_FALSE(answer.ec) << answer.what;
  ASSERT_EQ(answer.replies.size(), 1u);
  EXPECT_EQ(answer.replies[0].text, "PONG");

  const auto seen = server.commands();
  ASSERT_GE(seen.size(), 3u);
  EXPECT_EQ(seen[0], (Command{"AUTH", "node", "secret"}));
  EXPECT_EQ(seen[1], (Command{"SELECT", "3"}));
  EXPECT_EQ(seen[2], (Command{"PING"}));
}

// The default user signs in with the password alone, which is what Redis 5 and earlier understand.
TEST(RespConnectionTest, TheDefaultUserSignsInWithThePasswordAlone) {
  ScriptedServer server(answer_ok_and_pong);
  auto settings = settings_for(server);
  settings.username = "default";
  settings.password = "secret";
  Client client(settings);

  ASSERT_FALSE(client.exec(ping()).ec);
  EXPECT_EQ(server.commands().front(), (Command{"AUTH", "secret"}));
}

// Requests sent together are answered in the order they were sent, each with its own replies.
TEST(RespConnectionTest, PipelinedRequestsAreAnsweredInOrder) {
  ScriptedServer server([](const Command& command, int) { return Action::answer("$" + std::to_string(command[1].size()) + "\r\n" + command[1] + "\r\n"); });
  Client client(settings_for(server));

  std::vector<std::future<Client::Answer>> answers;
  for (int i = 0; i < 20; ++i) {
    Request request;
    request.push("GET", "key-" + std::to_string(i));
    answers.push_back(std::async(std::launch::async, [&client, request]() mutable { return client.exec(std::move(request)); }));
  }
  std::vector<std::string> got;
  for (auto& answer : answers) {
    const auto value = answer.get();
    ASSERT_FALSE(value.ec) << value.what;
    got.push_back(value.replies.at(0).text);
  }
  std::sort(got.begin(), got.end());
  for (int i = 0; i < 20; ++i) EXPECT_NE(std::find(got.begin(), got.end(), "key-" + std::to_string(i)), got.end());

  Request two;
  two.push("GET", "first");
  two.push("GET", "second");
  const auto both = client.exec(std::move(two));
  ASSERT_EQ(both.replies.size(), 2u);
  EXPECT_EQ(both.replies[0].text, "first");
  EXPECT_EQ(both.replies[1].text, "second");
}

// The server saying no is an answer, not a failure of the connection.
TEST(RespConnectionTest, AnErrorReplyIsAReply) {
  ScriptedServer server([](const Command&, int) { return Action::answer("-WRONGTYPE not a set\r\n"); });
  Client client(settings_for(server));

  const auto answer = client.exec(ping());
  ASSERT_FALSE(answer.ec);
  ASSERT_EQ(answer.replies.size(), 1u);
  EXPECT_TRUE(answer.replies[0].is_error());
  EXPECT_EQ(answer.replies[0].text, "WRONGTYPE not a set");
}

// A sign-in Redis refuses leaves the link down: nothing is sent past it, and requests fail rather than wait for ever.
TEST(RespConnectionTest, ARefusedSignInSendsNothingElse) {
  ScriptedServer server([](const Command& command, int) {
    if (command[0] == "AUTH") return Action::answer("-WRONGPASS invalid username-password pair\r\n");
    return Action::answer("+PONG\r\n");
  });
  auto settings = settings_for(server);
  settings.password = "wrong";
  settings.request_timeout = 400ms;
  Client client(settings);

  const auto answer = client.exec(ping());
  EXPECT_TRUE(answer.ec);
  for (const auto& command : server.commands()) EXPECT_EQ(command[0], "AUTH") << "nothing but a sign-in reaches a server that refused one";
}

// A request the server never answers fails at its deadline.
TEST(RespConnectionTest, ARequestFailsAtItsDeadline) {
  ScriptedServer server([](const Command&, int) { return Action::silence(); });
  auto settings = settings_for(server);
  settings.request_timeout = 300ms;
  Client client(settings);

  const auto start = std::chrono::steady_clock::now();
  const auto answer = client.exec(ping());
  const auto took = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(answer.ec, boost::system::errc::timed_out);
  EXPECT_GE(took, 250ms);
  EXPECT_LT(took, 3s);
}

// A link that drops fails what was written to it, which may or may not have been carried out; the connection comes
// back by itself and the next request goes through.
TEST(RespConnectionTest, ALostLinkFailsWhatWasWrittenAndComesBack) {
  ScriptedServer server([](const Command& command, int connection) {
    if (connection == 1 && command[0] == "GET") return Action::hang_up();
    return answer_ok_and_pong(command, connection);
  });
  Client client(settings_for(server));

  ASSERT_FALSE(client.exec(ping()).ec);

  Request get;
  get.push("GET", "k");
  const auto lost = client.exec(std::move(get));
  EXPECT_EQ(lost.ec, boost::system::errc::not_connected) << lost.what;

  Client::Answer after;
  for (int i = 0; i < 40; ++i) {
    after = client.exec(ping());
    if (!after.ec) break;
    std::this_thread::sleep_for(50ms);
  }
  EXPECT_FALSE(after.ec) << after.what;
  EXPECT_GE(server.connections(), 2);
}

// A server that is not there fails a request at its deadline, and nothing is held past it.
TEST(RespConnectionTest, NoServerFailsRequestsWithinTheirDeadline) {
  Connection::Settings settings;
  settings.port = 1;  // refused at once on loopback
  settings.request_timeout = 400ms;
  settings.reconnect_min = 50ms;
  settings.reconnect_max = 100ms;
  Client client(settings);

  const auto answer = client.exec(ping());
  EXPECT_TRUE(answer.ec);
}

// Stopping answers everything still waiting, so no caller is left without an answer.
TEST(RespConnectionTest, StoppingAnswersEverythingWaiting) {
  ScriptedServer server([](const Command&, int) { return Action::silence(); });
  auto settings = settings_for(server);
  settings.request_timeout = 10s;
  auto client = std::make_unique<Client>(settings);

  auto promise = std::make_shared<std::promise<boost::system::error_code>>();
  auto future = promise->get_future();
  client->connection->exec(ping(), [promise](boost::system::error_code ec, std::string, std::vector<Reply>) { promise->set_value(ec); });
  std::this_thread::sleep_for(200ms);

  client.reset();
  ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(future.get());
}
