//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "datastores/datastore.h"
#include "dns/sip_locator.h"
#include "loggers/logger.h"
#include "plugins/plugin.h"
#include "types/trunk.h"

namespace athenasip::cli {

// `athenasip --check`: whether this node, with this configuration, can reach everything it
// needs. Changes nothing and opens no listener, so it is safe beside a serving node.
struct CheckLine {
  bool ok = false;
  std::string what;
  std::string detail;
};

// How to wait for an async connect; main supplies it so checks run on its executor.
using ConnectAndWait = std::function<plugins::Status(const std::function<void(plugins::Executor, plugins::StatusHandler)>&)>;

// Every check, in start-up order. The configuration must already have loaded.
std::vector<CheckLine> check(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Config> config, const ConnectAndWait& connect_and_wait);

// Sends an OPTIONS for a trunk to one hop and answers the response's start line, or why there was none.
using TrunkProbe = std::function<plugins::Result<std::string>(const types::Trunk& trunk, const dns::Hop& hop)>;

// Each trunk's next hop, its proxy or else its URI, located as the node would locate it (RFC 3263): a trunk that
// resolves nowhere carries no call. With a probe, the first hop is also sent an OPTIONS (RFC 3261 11), and any
// response is an answer, a challenge included. No lines when the datastore keeps no trunks.
std::vector<CheckLine> check_trunks(const std::shared_ptr<datastores::Datastore>& datastore, const std::shared_ptr<dns::SipLocator>& locator,
                                    const TrunkProbe& probe = nullptr);

// The probe `athenasip --check-trunks` uses: from a socket of its own, so it runs beside a serving node. UDP is sent
// again at RFC 3261's intervals for four seconds; TLS verifies the carrier against the trunk's CA, or the system's.
plugins::Result<std::string> options_probe(const types::Trunk& trunk, const dns::Hop& hop);

// A URL with its password masked, for printing.
std::string redacted(const std::string& url);

// The lines as text, and whether all passed.
std::string report(const std::vector<CheckLine>& lines);
bool passed(const std::vector<CheckLine>& lines);

}  // namespace athenasip::cli
