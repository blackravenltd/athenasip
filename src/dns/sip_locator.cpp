//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "dns/sip_locator.h"

#include <openssl/rand.h>

#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>

#include "util.h"

namespace athenasip::dns {

namespace {

// RFC 3263 4.1 and RFC 3261 26: what each NAPTR service and SRV name means, for the
// transports this node can open. Order here is preference when DNS expresses none.
struct Service {
  const char* naptr;
  const char* srv_prefix;
  const char* transport;
  bool secure;
};

constexpr Service kServices[] = {
    {"SIP+D2U", "_sip._udp.", "udp", false},
    {"SIP+D2T", "_sip._tcp.", "tcp", false},
    {"SIPS+D2T", "_sips._tcp.", "tls", true},
};

const Service* service_for_naptr(const std::string& name) {
  for (const auto& service : kServices) {
    if (Util::to_upper(name) == service.naptr) return &service;
  }
  return nullptr;
}

const Service* service_for_transport(const std::string& transport, bool secure) {
  for (const auto& service : kServices) {
    if (transport == service.transport && (secure == service.secure || transport == "tls")) return &service;
  }
  return nullptr;
}

std::uint32_t default_random(std::uint32_t total) {
  std::uint32_t value = 0;
  RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value));
  return total == 0 ? 0 : value % (total + 1);
}

bool is_root(const std::string& name) { return name.empty() || name == "."; }

}  // namespace

struct SipLocator::Search {
  plugins::Executor on;
  plugins::Handler<std::vector<Hop>> handler;

  std::string host;
  bool secure = false;

  // What the URI fixed, when it fixed it.
  std::string transport;
  std::uint16_t port = 0;

  std::vector<Hop> hops;

  // Whether DNS answered anything at all. A search that heard nothing from anybody is a
  // failure; one told "nothing here" has an answer, and it is empty.
  bool answered = false;
  bool srv_found = false;
};

SipLocator::SipLocator(std::shared_ptr<Resolver> resolver, Random random)
    : _resolver(std::move(resolver)), _random(random ? std::move(random) : Random(default_random)) {}

void SipLocator::locate(plugins::Executor on, const types::SIPUri& uri, plugins::Handler<std::vector<Hop>> handler) {
  auto search = std::make_shared<Search>();
  search->on = std::move(on);
  search->handler = std::move(handler);
  search->secure = Util::to_lower(uri.scheme) == "sips";

  search->host = uri.host;
  if (search->host.size() > 2 && search->host.front() == '[' && search->host.back() == ']') search->host = search->host.substr(1, search->host.size() - 2);

  search->transport = Util::to_lower(uri.parameter("transport"));
  if (search->secure && (search->transport.empty() || search->transport == "tcp")) search->transport = search->transport.empty() ? "" : "tls";
  if (uri.port) search->port = *uri.port;

  const auto default_port = [&search]() -> std::uint16_t { return search->secure || search->transport == "tls" ? 5061 : 5060; };
  const auto default_transport = [&search]() -> std::string { return search->secure ? "tls" : "udp"; };

  // 4.1 and 4.2: a numeric host is already the address.
  boost::system::error_code ec;
  const auto literal = boost::asio::ip::make_address(search->host, ec);
  if (!ec) {
    search->answered = true;
    search->hops.push_back(
        Hop{search->transport.empty() ? default_transport() : search->transport, literal.to_string(), search->port ? search->port : default_port()});
    return _finish(search);
  }

  // RFC 6761 6.4: nothing under .invalid exists, and asking would only tell the network
  // about it. Browsers name their Contact this way, and so does AthenaPhone.
  const auto lowered = Util::to_lower(search->host);
  const std::string invalid = ".invalid";
  if (lowered == "invalid" || (lowered.size() > invalid.size() && lowered.compare(lowered.size() - invalid.size(), invalid.size(), invalid) == 0)) {
    search->answered = true;
    return _finish(search);
  }

  // 4.2: a port in the URI means the host is to be looked up as an address, nothing more.
  if (search->port) {
    if (search->transport.empty()) search->transport = default_transport();
    return _addresses(search, {{search->host, Srv{0, 0, search->port, search->host}}}, 0);
  }

  // 4.1 and 4.2: a transport parameter fixes the transport, and SRV for it still decides
  // where.
  if (!search->transport.empty()) {
    const auto* service = service_for_transport(search->transport, search->secure);
    if (!service) {
      search->answered = true;
      return _finish(search);
    }
    return _srv_in_turn(search, {{std::string(service->srv_prefix) + search->host, service->transport}}, 0);
  }

  _naptr(search);
}

void SipLocator::_naptr(std::shared_ptr<Search> search) {
  auto self = shared_from_this();

  _resolver->query(search->on, search->host, Type::NAPTR, [self, search](plugins::Result<std::vector<Record>> answer) {
    if (answer.ok) search->answered = true;

    // 4.1: keep what this client can use - for a sips URI only SIPS+D2T - and sort by order
    // then preference. "s" is the only flag that leads to SRV; anything else is not a
    // record for this procedure.
    std::vector<Naptr> usable;
    if (answer.ok) {
      for (const auto& record : answer.value) {
        const auto* service = service_for_naptr(record.naptr.services);
        if (!service || (search->secure && !service->secure)) continue;
        if (Util::to_lower(record.naptr.flags) != "s" || is_root(record.naptr.replacement)) continue;
        usable.push_back(record.naptr);
      }
    }

    std::stable_sort(usable.begin(), usable.end(),
                     [](const Naptr& a, const Naptr& b) { return a.order != b.order ? a.order < b.order : a.preference < b.preference; });

    std::vector<std::pair<std::string, std::string>> names;
    for (const auto& naptr : usable) names.emplace_back(naptr.replacement, service_for_naptr(naptr.services)->transport);

    // 4.1: no usable NAPTR, so SRV for each transport this client supports.
    if (names.empty()) {
      for (const auto& service : kServices) {
        if (search->secure && !service.secure) continue;
        names.emplace_back(std::string(service.srv_prefix) + search->host, service.transport);
      }
    }

    self->_srv_in_turn(search, std::move(names), 0);
  });
}

void SipLocator::_srv_in_turn(std::shared_ptr<Search> search, std::vector<std::pair<std::string, std::string>> names, std::size_t next) {
  // Every SRV name asked. If none had records, 4.2: the host's own addresses, on the
  // default port and the default transport (or the one the URI fixed).
  if (next >= names.size()) {
    if (search->srv_found) return _finish(search);

    if (search->transport.empty()) search->transport = search->secure ? "tls" : "udp";
    const std::uint16_t port = search->secure || search->transport == "tls" ? 5061 : 5060;
    return _addresses(search, {{search->host, Srv{0, 0, port, search->host}}}, 0);
  }

  auto self = shared_from_this();
  const auto [name, transport] = names[next];

  _resolver->query(search->on, name, Type::SRV, [self, search, names, next, transport](plugins::Result<std::vector<Record>> answer) mutable {
    if (!answer.ok || answer.value.empty()) {
      if (answer.ok) search->answered = true;
      return self->_srv_in_turn(search, std::move(names), next + 1);
    }

    search->answered = true;
    search->srv_found = true;

    std::vector<Srv> records;
    for (const auto& record : answer.value) records.push_back(record.srv);

    std::vector<std::pair<std::string, Srv>> targets;
    for (const auto& srv : self->_order(std::move(records))) {
      // RFC 2782: "." is the domain saying it does not offer this at all.
      if (is_root(srv.target)) continue;
      targets.emplace_back(transport, srv);
    }

    // The addresses of these targets, then on to the next SRV name - the next transport
    // NAPTR listed, or the next this client supports.
    self->_srv_targets(search, std::move(targets), 0, std::move(names), next + 1);
  });
}

void SipLocator::_srv_targets(std::shared_ptr<Search> search, std::vector<std::pair<std::string, Srv>> targets, std::size_t index,
                              std::vector<std::pair<std::string, std::string>> names, std::size_t next) {
  if (index >= targets.size()) return _srv_in_turn(search, std::move(names), next);

  auto self = shared_from_this();
  const auto [transport, srv] = targets[index];

  _resolver->query(
      search->on, srv.target, Type::A,
      [self, search, targets = std::move(targets), index, names = std::move(names), next, transport, srv](plugins::Result<std::vector<Record>> v4) mutable {
        self->_resolver->query(search->on, srv.target, Type::AAAA,
                               [self, search, targets = std::move(targets), index, names = std::move(names), next, transport, srv,
                                v4](plugins::Result<std::vector<Record>> v6) mutable {
                                 if (v4.ok) {
                                   for (const auto& record : v4.value) search->hops.push_back(Hop{transport, record.address, srv.port});
                                 }
                                 if (v6.ok) {
                                   for (const auto& record : v6.value) search->hops.push_back(Hop{transport, record.address, srv.port});
                                 }
                                 self->_srv_targets(search, std::move(targets), index + 1, std::move(names), next);
                               });
      });
}

void SipLocator::_addresses(std::shared_ptr<Search> search, std::vector<std::pair<std::string, Srv>> targets, std::size_t next) {
  if (next >= targets.size()) return _finish(search);

  auto self = shared_from_this();
  const auto host = targets[next].first;
  const auto port = targets[next].second.port;

  _resolver->query(search->on, host, Type::A, [self, search, targets, next, host, port](plugins::Result<std::vector<Record>> v4) mutable {
    self->_resolver->query(search->on, host, Type::AAAA, [self, search, targets, next, port, v4](plugins::Result<std::vector<Record>> v6) mutable {
      if (v4.ok || v6.ok) search->answered = true;

      if (v4.ok) {
        for (const auto& record : v4.value) search->hops.push_back(Hop{search->transport, record.address, port});
      }
      if (v6.ok) {
        for (const auto& record : v6.value) search->hops.push_back(Hop{search->transport, record.address, port});
      }

      self->_addresses(search, std::move(targets), next + 1);
    });
  });
}

void SipLocator::_finish(std::shared_ptr<Search> search) {
  auto result = !search->answered && search->hops.empty() ? plugins::Result<std::vector<Hop>>::failure("no answer from DNS for " + search->host)
                                                          : plugins::Result<std::vector<Hop>>::success(search->hops);

  boost::asio::post(search->on, [handler = search->handler, result = std::move(result)]() mutable { handler(std::move(result)); });
}

// RFC 2782: by priority, lowest first; within a priority, zero weights first in the list,
// then repeatedly a number in [0, sum of weights] chosen at random and the first record
// whose running sum reaches it taken out.
std::vector<Srv> SipLocator::_order(std::vector<Srv> records) const {
  std::stable_sort(records.begin(), records.end(), [](const Srv& a, const Srv& b) { return a.priority < b.priority; });

  std::vector<Srv> ordered;
  std::size_t start = 0;

  while (start < records.size()) {
    std::size_t end = start;
    while (end < records.size() && records[end].priority == records[start].priority) ++end;

    std::vector<Srv> group(records.begin() + static_cast<std::ptrdiff_t>(start), records.begin() + static_cast<std::ptrdiff_t>(end));
    std::stable_partition(group.begin(), group.end(), [](const Srv& srv) { return srv.weight == 0; });

    while (!group.empty()) {
      std::uint32_t total = 0;
      for (const auto& srv : group) total += srv.weight;

      const auto pick = _random(total);

      std::uint32_t running = 0;
      std::size_t chosen = group.size() - 1;
      for (std::size_t i = 0; i < group.size(); ++i) {
        running += group[i].weight;
        if (running >= pick) {
          chosen = i;
          break;
        }
      }

      ordered.push_back(group[chosen]);
      group.erase(group.begin() + static_cast<std::ptrdiff_t>(chosen));
    }

    start = end;
  }

  return ordered;
}

}  // namespace athenasip::dns
