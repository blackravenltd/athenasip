//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../loggers/logger.h"
#include "../plugins/plugin.h"
#include "../plugins/plugin_registry.h"

namespace athenasip::push {

inline constexpr char kind[] = "push";

// RFC 8599: what a binding's Contact carries to be woken, and why it is being woken.
struct Notification {
  enum class Reason {
    // A request for a new dialog, or a standalone request, is waiting for the client (5.6.2).
    Request,
    // The binding is about to expire and the client should refresh it (5.5).
    Refresh,
  };

  std::string provider;  // pn-provider
  std::string prid;      // pn-prid
  std::string param;     // pn-param, empty when the binding has none
  Reason reason = Reason::Request;
};

// A push notification service (PNS). A driver's name() is its pn-provider value ("apns",
// "fcm", "webpush"), and a node may run several, one per provider.
class PushService : public plugins::Plugin {
 public:
  std::string kind() const final { return push::kind; }

  // Credentials are read in configure(); connect() is for anything that needs the network
  // before the first push. Most drivers need nothing.
  virtual void connect(plugins::Executor on, plugins::StatusHandler handler) { _complete(std::move(on), std::move(handler), plugins::Status::success()); }
  virtual void close() {}

  // Whether a binding carries everything this provider needs (RFC 8599 sections 10 to 12):
  // apns and fcm need a pn-param, webpush must not have one.
  virtual bool accepts(const Notification& notification) const = 0;

  // Whether a push asking the client to refresh its binding (RFC 8599 5.5) may be sent to it. False where the
  // service punishes a push that does not ring, as iOS does for VoIP pushes; that client refreshes on its own.
  virtual bool refreshes(const Notification& notification) const {
    (void)notification;
    return true;
  }

  // Feature-capability indicators this provider adds beside +sip.pns in a 2xx to REGISTER
  // (RFC 8599 5.4), as name and value: {"+sip.vapid", "<key>"}. Empty for most.
  virtual std::vector<std::pair<std::string, std::string>> capabilities() const { return {}; }

  // Asks the provider to wake the client. Success means the provider took the request, not
  // that the client woke. Called on the node's strand; the handler is posted back to `on`.
  virtual void send(plugins::Executor on, Notification notification, plugins::StatusHandler handler) = 0;

  template <typename T, typename = std::enable_if_t<std::is_base_of_v<PushService, T>>>
  static void register_driver(std::shared_ptr<loggers::Logger> logger, std::string scheme) {
    plugins::PluginRegistry::instance().add<T>(std::move(logger), push::kind, std::move(scheme));
  }

  static std::shared_ptr<PushService> create_driver(std::shared_ptr<loggers::Logger> logger, const std::string& url_string) {
    return plugins::PluginRegistry::instance().create_as<PushService>(std::move(logger), push::kind, url_string);
  }
};

}  // namespace athenasip::push
