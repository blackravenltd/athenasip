//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "dns/sip_locator.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <future>
#include <map>
#include <mutex>

#include "../helpers/fake_resolver_helper.h"
#include "../mocks/logger_mock.h"
#include "global_io_context.h"

using namespace athenasip;
using namespace athenasip::dns;

namespace {

plugins::Result<std::vector<Hop>> locate(const std::shared_ptr<FakeResolver>& resolver, const std::string& uri, SipLocator::Random random = {}) {
  auto locator = std::make_shared<SipLocator>(resolver, std::move(random));

  std::promise<plugins::Result<std::vector<Hop>>> promise;
  auto future = promise.get_future();
  locator->locate(detail::get_global_io_context().get_executor(), types::SIPUri(uri),
                  [&promise](plugins::Result<std::vector<Hop>> result) { promise.set_value(std::move(result)); });

  if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    ADD_FAILURE() << "the locator never answered";
    return plugins::Result<std::vector<Hop>>::failure("never answered");
  }
  return future.get();
}

Hop hop(const std::string& transport, const std::string& address, std::uint16_t port) { return Hop{transport, address, port}; }

}  // namespace

// 4.1 and 4.2: a numeric host is already an address. No DNS at all.
TEST(SipLocatorTest, ANumericHostNeedsNoDns) {
  auto resolver = std::make_shared<FakeResolver>();

  const auto hops = locate(resolver, "sip:bob@192.0.2.9");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, std::vector<Hop>{hop("udp", "192.0.2.9", 5060)});
  EXPECT_TRUE(resolver->asked.empty());
}

TEST(SipLocatorTest, ANumericHostTakesItsTransportAndPortFromTheUri) {
  auto resolver = std::make_shared<FakeResolver>();

  const auto hops = locate(resolver, "sip:bob@192.0.2.9:5080;transport=tcp");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, std::vector<Hop>{hop("tcp", "192.0.2.9", 5080)});
}

// 4.1: "if the URI ... is a SIPS URI ... TCP" - TLS over it - and 5061.
TEST(SipLocatorTest, ASipsUriWithANumericHostIsTlsOn5061) {
  auto resolver = std::make_shared<FakeResolver>();

  const auto hops = locate(resolver, "sips:bob@192.0.2.9");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, std::vector<Hop>{hop("tls", "192.0.2.9", 5061)});
}

TEST(SipLocatorTest, AnIpv6LiteralNeedsNoDnsEither) {
  auto resolver = std::make_shared<FakeResolver>();

  const auto hops = locate(resolver, "sip:bob@[2001:db8::9]:5062");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, std::vector<Hop>{hop("udp", "2001:db8::9", 5062)});
  EXPECT_TRUE(resolver->asked.empty());
}

// 4.2: "If the TARGET was not a numeric IP address, but a port is present in the URI, the
// client performs an A or AAAA record lookup of the domain name." No NAPTR, no SRV.
TEST(SipLocatorTest, AnExplicitPortSkipsNaptrAndSrv) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->a("pbx.example.com", "198.51.100.5");
  resolver->naptr("pbx.example.com", 10, 10, "SIP+D2T", "_sip._tcp.pbx.example.com");

  const auto hops = locate(resolver, "sip:bob@pbx.example.com:5070");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, std::vector<Hop>{hop("udp", "198.51.100.5", 5070)});
  EXPECT_FALSE(resolver->was_asked("pbx.example.com", Type::NAPTR));
  EXPECT_FALSE(resolver->was_asked("_sip._udp.pbx.example.com", Type::SRV));
}

// 4.1 and 4.2: a transport parameter decides the transport; SRV for it still decides the
// port and the hosts.
TEST(SipLocatorTest, ATransportParameterSkipsNaptrButNotSrv) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->srv("_sip._tcp.example.com", 10, 0, 5090, "edge.example.com");
  resolver->a("edge.example.com", "198.51.100.6");

  const auto hops = locate(resolver, "sip:bob@example.com;transport=tcp");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, std::vector<Hop>{hop("tcp", "198.51.100.6", 5090)});
  EXPECT_FALSE(resolver->was_asked("example.com", Type::NAPTR));
}

// 4.1: NAPTR, in order then preference, keeping only services this client can use; each
// replacement is an SRV name. sip2sip.info's real records, in a shuffled order to show
// the sorting is the locator's and not the server's.
TEST(SipLocatorTest, NaptrDecidesTheTransportsInOrder) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->naptr("sip2sip.info", 25, 100, "SIPS+D2T", "_sips._tcp.sip2sip.info");
  resolver->naptr("sip2sip.info", 10, 100, "SIP+D2T", "_sip._tcp.sip2sip.info");
  resolver->naptr("sip2sip.info", 20, 100, "SIP+D2U", "_sip._udp.sip2sip.info");
  resolver->srv("_sip._tcp.sip2sip.info", 100, 100, 5060, "proxy.sipthor.net");
  resolver->srv("_sip._udp.sip2sip.info", 100, 100, 5060, "proxy.sipthor.net");
  resolver->srv("_sips._tcp.sip2sip.info", 100, 100, 5061, "proxy.sipthor.net");
  resolver->a("proxy.sipthor.net", "81.23.228.129");

  const auto hops = locate(resolver, "sip:alice@sip2sip.info");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, (std::vector<Hop>{hop("tcp", "81.23.228.129", 5060), hop("udp", "81.23.228.129", 5060), hop("tls", "81.23.228.129", 5061)}));
}

// 4.1: "If the URI is a SIPS URI, only those records with a service field of SIPS+D2T are
// retained."
TEST(SipLocatorTest, ASipsUriKeepsOnlySipsNaptrRecords) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->naptr("sip2sip.info", 10, 100, "SIP+D2T", "_sip._tcp.sip2sip.info");
  resolver->naptr("sip2sip.info", 25, 100, "SIPS+D2T", "_sips._tcp.sip2sip.info");
  resolver->srv("_sip._tcp.sip2sip.info", 100, 100, 5060, "proxy.sipthor.net");
  resolver->srv("_sips._tcp.sip2sip.info", 100, 100, 5061, "proxy.sipthor.net");
  resolver->a("proxy.sipthor.net", "81.23.228.129");

  const auto hops = locate(resolver, "sips:alice@sip2sip.info");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, std::vector<Hop>{hop("tls", "81.23.228.129", 5061)});
}

// A service this client cannot speak is not one to try. SIP over WebSocket (RFC 7118) is
// a transport this node accepts and cannot open.
TEST(SipLocatorTest, ServicesThisNodeCannotUseAreDropped) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->naptr("example.com", 5, 10, "SIP+D2W", "_sip._ws.example.com");
  resolver->naptr("example.com", 10, 10, "SIP+D2U", "_sip._udp.example.com");
  resolver->srv("_sip._udp.example.com", 0, 0, 5060, "edge.example.com");
  resolver->a("edge.example.com", "198.51.100.7");

  const auto hops = locate(resolver, "sip:bob@example.com");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, std::vector<Hop>{hop("udp", "198.51.100.7", 5060)});
  EXPECT_FALSE(resolver->was_asked("_sip._ws.example.com", Type::SRV));
}

// 4.1: "If no NAPTR records are found, the client constructs SRV queries for those
// transport protocols it supports."
TEST(SipLocatorTest, NoNaptrMeansSrvForEachTransport) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->srv("_sip._udp.iptel.org", 0, 25, 5060, "sip.iptel.org");
  resolver->a("sip.iptel.org", "212.79.111.155");

  const auto hops = locate(resolver, "sip:alice@iptel.org");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, std::vector<Hop>{hop("udp", "212.79.111.155", 5060)});
  EXPECT_TRUE(resolver->was_asked("_sip._tcp.iptel.org", Type::SRV));
}

// 4.2: "If no SRV records were found, the client SHOULD use ... A or AAAA" with the
// default port, UDP for sip.
TEST(SipLocatorTest, NoNaptrAndNoSrvMeansTheAddressesOnTheDefaultPort) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->a("pbx.example.com", "198.51.100.5");
  resolver->aaaa("pbx.example.com", "2001:db8::5");

  const auto hops = locate(resolver, "sip:bob@pbx.example.com");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, (std::vector<Hop>{hop("udp", "198.51.100.5", 5060), hop("udp", "2001:db8::5", 5060)}));
}

// RFC 2782: lower priority first, whatever order the server gave them in.
TEST(SipLocatorTest, SrvTargetsAreTriedInPriorityOrder) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->srv("_sip._udp.example.com", 20, 0, 5060, "backup.example.com");
  resolver->srv("_sip._udp.example.com", 10, 0, 5060, "primary.example.com");
  resolver->a("primary.example.com", "198.51.100.1");
  resolver->a("backup.example.com", "198.51.100.2");

  const auto hops = locate(resolver, "sip:bob@example.com");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, (std::vector<Hop>{hop("udp", "198.51.100.1", 5060), hop("udp", "198.51.100.2", 5060)}));
}

// RFC 2782's selection within one priority: sum the weights, pick a number in [0, sum],
// take the first record whose running sum reaches it, remove it and repeat. Here the dice
// say 70 of 0..100 the first time, which falls in the second record's range.
TEST(SipLocatorTest, WithinAPriorityTheWeightsDecide) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->srv("_sip._udp.example.com", 10, 60, 5060, "big.example.com");
  resolver->srv("_sip._udp.example.com", 10, 40, 5060, "small.example.com");
  resolver->a("big.example.com", "198.51.100.1");
  resolver->a("small.example.com", "198.51.100.2");

  const auto hops = locate(resolver, "sip:bob@example.com", [](std::uint32_t total) { return total == 100 ? 70u : 0u; });

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, (std::vector<Hop>{hop("udp", "198.51.100.2", 5060), hop("udp", "198.51.100.1", 5060)}));
}

// RFC 2782: "A Target of '.' means that the service is decidedly not available at this
// domain."
TEST(SipLocatorTest, ATargetOfDotIsNoServiceAtAll) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->srv("_sip._udp.example.com", 0, 0, 0, ".");
  resolver->srv("_sip._udp.example.com", 0, 0, 0, "");

  const auto hops = locate(resolver, "sip:bob@example.com");

  ASSERT_TRUE(hops.ok);
  EXPECT_TRUE(hops.value.empty());
  EXPECT_FALSE(resolver->was_asked("example.com", Type::A)) << "an SRV that says no is an answer, not a reason to guess";
}

TEST(SipLocatorTest, ANameThatDoesNotExistIsAnEmptyAnswer) {
  auto resolver = std::make_shared<FakeResolver>();

  const auto hops = locate(resolver, "sip:bob@nowhere.example");

  ASSERT_TRUE(hops.ok);
  EXPECT_TRUE(hops.value.empty());
}

// RFC 6761 section 6.4: a name under .invalid does not exist, and a resolver should say so
// without asking anybody. Clients put such names in their Contact on purpose - a browser
// always, AthenaPhone on every transport - so this is asked often.
TEST(SipLocatorTest, ANameUnderInvalidIsAnsweredWithoutAskingDns) {
  auto resolver = std::make_shared<FakeResolver>();

  const auto hops = locate(resolver, "sip:bob@df7jal23ls0d.invalid;transport=tcp");

  ASSERT_TRUE(hops.ok);
  EXPECT_TRUE(hops.value.empty());
  EXPECT_TRUE(resolver->asked.empty());
}

// Against the real DNS, through this machine's own nameservers, when ATHENA_TEST_DNS is
// set: the resolver, the codec and the locator together, on a domain that publishes the
// full RFC 3263 set. Opt-in like the Redis and MQTT tests, because it needs the network.
TEST(SipLocatorTest, ARealDomainIsLocatedThroughTheSystemResolver) {
  if (!std::getenv("ATHENA_TEST_DNS")) GTEST_SKIP() << "set ATHENA_TEST_DNS=1 to resolve against the real DNS";

  auto servers = UdpResolver::servers_from("/etc/resolv.conf");
  ASSERT_FALSE(servers.empty());

  auto locator = std::make_shared<SipLocator>(std::make_shared<UdpResolver>(std::make_shared<MockLogger>(), servers));

  std::promise<plugins::Result<std::vector<Hop>>> promise;
  auto future = promise.get_future();
  locator->locate(detail::get_global_io_context().get_executor(), types::SIPUri("sip:alice@sip2sip.info"),
                  [&promise](plugins::Result<std::vector<Hop>> result) { promise.set_value(std::move(result)); });
  ASSERT_EQ(future.wait_for(std::chrono::seconds(20)), std::future_status::ready);

  const auto hops = future.get();
  ASSERT_TRUE(hops.ok) << hops.error;
  ASSERT_FALSE(hops.value.empty());

  // NAPTR order 10 is TCP, so that comes first; the rest follow in NAPTR order.
  EXPECT_EQ(hops.value.front().transport, "tcp");
  EXPECT_EQ(hops.value.front().port, 5060);

  bool tls = false;
  for (const auto& hop : hops.value) tls = tls || (hop.transport == "tls" && hop.port == 5061);
  std::string seen;
  for (const auto& hop : hops.value) seen += hop.transport + "://" + hop.address + ":" + std::to_string(hop.port) + " ";
  EXPECT_TRUE(tls) << seen;
}

// No answer from DNS is not "nothing there": it is not knowing, and the caller is told so.
TEST(SipLocatorTest, NoAnswerAtAllIsAFailure) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->failing = {"pbx.example.com/1", "pbx.example.com/28"};

  const auto hops = locate(resolver, "sip:bob@pbx.example.com:5070");

  EXPECT_FALSE(hops.ok);
}

// But a failure at one step of several is not the end: an unanswered NAPTR question moves
// on to SRV, as an empty one would.
TEST(SipLocatorTest, AnUnansweredNaptrMovesOnToSrv) {
  auto resolver = std::make_shared<FakeResolver>();
  resolver->failing = {"example.com/35"};
  resolver->srv("_sip._udp.example.com", 0, 0, 5060, "edge.example.com");
  resolver->a("edge.example.com", "198.51.100.7");

  const auto hops = locate(resolver, "sip:bob@example.com");

  ASSERT_TRUE(hops.ok);
  EXPECT_EQ(hops.value, std::vector<Hop>{hop("udp", "198.51.100.7", 5060)});
}
