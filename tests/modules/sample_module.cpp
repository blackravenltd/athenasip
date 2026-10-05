//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// A plugin module as a third party would build one: a push service named "sample", which accepts any binding
// with a pn-prid and says it sent every push.
#include <memory>
#include <string>
#include <utility>

#include "plugins/plugin_module.h"
#include "push/push_service.h"

namespace {

class SamplePushService : public athenasip::push::PushService {
 public:
  SamplePushService(std::shared_ptr<athenasip::loggers::Logger> logger, std::shared_ptr<athenasip::types::URL> url) {
    (void)logger;
    (void)url;
  }

  std::string name() const override { return "sample"; }
  std::string version() const override { return "1.2.3"; }

  bool accepts(const athenasip::push::Notification& notification) const override { return !notification.prid.empty(); }

  void send(athenasip::plugins::Executor on, athenasip::push::Notification notification, athenasip::plugins::StatusHandler handler) override {
    (void)notification;
    _complete(std::move(on), std::move(handler), athenasip::plugins::Status::success());
  }
};

void register_sample(athenasip::plugins::ModuleHost& host) { host.add<SamplePushService>(athenasip::push::kind, "sample"); }

}  // namespace

ATHENASIP_PLUGIN_MODULE("sample", register_sample)
