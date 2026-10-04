//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <boost/asio/post.hpp>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dns/resolver.h"

// Canned DNS: answers by name and type, fails where a test says so, and records every question asked.
class FakeResolver : public athenasip::dns::Resolver {
 public:
  void query(athenasip::plugins::Executor on, std::string name, athenasip::dns::Type type,
             athenasip::plugins::Handler<std::vector<athenasip::dns::Record>> handler) override {
    athenasip::plugins::Result<std::vector<athenasip::dns::Record>> result = athenasip::plugins::Result<std::vector<athenasip::dns::Record>>::success({});
    {
      std::lock_guard lock(_mutex);
      asked.push_back(name + "/" + std::to_string(static_cast<int>(type)));
      const auto key = name + "/" + std::to_string(static_cast<int>(type));
      if (failing.count(key))
        result = athenasip::plugins::Result<std::vector<athenasip::dns::Record>>::failure("no answer");
      else if (records.count(key))
        result = athenasip::plugins::Result<std::vector<athenasip::dns::Record>>::success(records[key]);
    }
    boost::asio::post(on, [handler, result]() { handler(result); });
  }

  void naptr(const std::string& name, std::uint16_t order, std::uint16_t preference, const std::string& service, const std::string& replacement) {
    athenasip::dns::Record record;
    record.name = name;
    record.type = athenasip::dns::Type::NAPTR;
    record.naptr.order = order;
    record.naptr.preference = preference;
    record.naptr.flags = "s";
    record.naptr.services = service;
    record.naptr.replacement = replacement;
    records[name + "/35"].push_back(record);
  }

  void srv(const std::string& name, std::uint16_t priority, std::uint16_t weight, std::uint16_t port, const std::string& target) {
    athenasip::dns::Record record;
    record.name = name;
    record.type = athenasip::dns::Type::SRV;
    record.srv = athenasip::dns::Srv{priority, weight, port, target};
    records[name + "/33"].push_back(record);
  }

  void a(const std::string& name, const std::string& address) {
    athenasip::dns::Record record;
    record.name = name;
    record.type = athenasip::dns::Type::A;
    record.address = address;
    records[name + "/1"].push_back(record);
  }

  void aaaa(const std::string& name, const std::string& address) {
    athenasip::dns::Record record;
    record.name = name;
    record.type = athenasip::dns::Type::AAAA;
    record.address = address;
    records[name + "/28"].push_back(record);
  }

  bool was_asked(const std::string& name, athenasip::dns::Type type) {
    std::lock_guard lock(_mutex);
    const auto key = name + "/" + std::to_string(static_cast<int>(type));
    return std::find(asked.begin(), asked.end(), key) != asked.end();
  }

  std::map<std::string, std::vector<athenasip::dns::Record>> records;
  std::set<std::string> failing;
  std::vector<std::string> asked;

 private:
  std::mutex _mutex;
};
