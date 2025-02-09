//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>

#include "logger_stdio.h"
#include "registrar.h"

using namespace athenasip;

#include <boost/asio/signal_set.hpp>
#include <iostream>
#include <optional>
#include <string>

#include "config.h"
#include "logger_scoped.h"
#include "logger_stdio.h"
#include "tls_server.h"
#include "url.h"
#include "util.h"
#include "version.h"

#include <mysqlx/xdevapi.h>

int main(int argc, char* argv[]) {
  auto mainLogger = std::make_shared<LoggerStdIO>(LogLevel::DEBUG);

  // This is v0.1.0
  Version version(0, 1, 0);

  mainLogger->raw("-----------------------------------");
  mainLogger->raw(" AthenaSIP v" + version.to_string());
  mainLogger->raw("-----------------------------------");

  Config config(mainLogger, argc, argv);

  if (!config.is_valid()) {
    mainLogger->error("Invalid Configuration");
    return 99;
  }

  std::shared_ptr<mysqlx::Session> session(std::make_shared<mysqlx::Session>("mysqlx://root@localhost/athenasip"));
  Registrar reg(mainLogger, session);

  reg.user_exists(SIPIdentity("Tom Cully <sip:tom@sip.blackraven.co.nz>"));

  // Create the tlsServer instance with the logger and start it on the specified port
  TLSServer tlsServer(mainLogger, 5061);
  Server& server = tlsServer;

  // Wait for SIGINT
  boost::asio::io_context signal_wait_context;
  boost::asio::signal_set signals(signal_wait_context, SIGINT);

  signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
    if (!error) {
      mainLogger->debug("SIGINT received");
      server.stop();
    }
  });

  // Set Certificates
  tlsServer.set_certificates("../tls/snakeoil.cer", "../tls/snakeoil.key");

  // Start TCP server
  server.start();

  // Do the SIGINT Wait
  signal_wait_context.run();
}
