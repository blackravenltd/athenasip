//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "pidf.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace athenasip;

namespace {

std::string document(const std::string& children, const std::string& extra_namespaces = "") {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<presence xmlns=\"urn:ietf:params:xml:ns:pidf\"" + extra_namespaces +
         " entity=\"sip:alice@example.com\">" + children + "</presence>";
}

std::string tuple(const std::string& id, const std::string& basic, const std::string& inside = "") {
  return "<tuple id=\"" + id + "\"><status><basic>" + basic + "</basic></status>" + inside + "</tuple>";
}

std::size_t count(const std::string& text, const std::string& what) {
  std::size_t n = 0;
  for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

}  // namespace

// RFC 3863 4.1: one presence document for the presentity, in the PIDF namespace.
TEST(PidfTest, TheComposedDocumentIsThePresentitys) {
  const auto composed = pidf::compose("sip:alice@example.com", {document(tuple("phone", "open"))});
  EXPECT_EQ(composed.rfind("<?xml", 0), 0u);
  EXPECT_NE(composed.find("<presence xmlns=\"urn:ietf:params:xml:ns:pidf\""), std::string::npos);
  EXPECT_NE(composed.find("entity=\"sip:alice@example.com\""), std::string::npos);
  EXPECT_NE(composed.find(tuple("phone", "open")), std::string::npos);
}

// docs/design/presence.md: two publications from two devices are two tuples.
TEST(PidfTest, EachPublicationsTuplesAreCarried) {
  const auto composed = pidf::compose("sip:alice@example.com", {document(tuple("phone", "open")), document(tuple("desk", "closed"))});
  EXPECT_NE(composed.find(tuple("phone", "open")), std::string::npos);
  EXPECT_NE(composed.find(tuple("desk", "closed")), std::string::npos);
}

// A tuple id in two publications is kept from the later one.
TEST(PidfTest, ATupleInTwoPublicationsIsTakenFromTheLater) {
  const auto composed = pidf::compose("sip:alice@example.com", {document(tuple("phone", "open")), document(tuple("phone", "closed"))});
  EXPECT_EQ(count(composed, "<tuple"), 1u);
  EXPECT_NE(composed.find(tuple("phone", "closed")), std::string::npos);
}

// RFC 3863 4.1: tuples, then notes, then extensions; each carried as published, with the namespaces it was
// published under, so an RPID element still means what it meant.
TEST(PidfTest, NotesAndExtensionsAreCarriedWithTheirNamespaces) {
  const std::string person = "<dm:person id=\"p1\"><rpid:activities><rpid:on-the-phone/></rpid:activities></dm:person>";
  const auto composed = pidf::compose("sip:alice@example.com",
                                      {document("<note>Back at 3</note>" + person + tuple("phone", "open"),
                                                " xmlns:dm=\"urn:ietf:params:xml:ns:pidf:data-model\" xmlns:rpid=\"urn:ietf:params:xml:ns:pidf:rpid\"")});

  EXPECT_NE(composed.find("xmlns:dm=\"urn:ietf:params:xml:ns:pidf:data-model\""), std::string::npos);
  EXPECT_NE(composed.find("xmlns:rpid=\"urn:ietf:params:xml:ns:pidf:rpid\""), std::string::npos);
  const auto at_tuple = composed.find("<tuple");
  const auto at_note = composed.find("<note>Back at 3</note>");
  const auto at_person = composed.find(person);
  ASSERT_NE(at_tuple, std::string::npos);
  ASSERT_NE(at_note, std::string::npos);
  ASSERT_NE(at_person, std::string::npos);
  EXPECT_LT(at_tuple, at_note);
  EXPECT_LT(at_note, at_person);
}

// Comments, processing instructions and nested elements of the same name do not confuse where an element ends.
TEST(PidfTest, AnElementEndsWhereItsOwnTagCloses) {
  const std::string nested =
      "<tuple id=\"t1\"><!-- <tuple> --><status><basic>open</basic></status><x:tuple xmlns:x=\"urn:x\"/><contact>sip:a@b</contact></tuple>";
  const auto composed = pidf::compose("sip:alice@example.com", {document(nested + tuple("t2", "closed"))});
  EXPECT_NE(composed.find(nested), std::string::npos);
  EXPECT_NE(composed.find(tuple("t2", "closed")), std::string::npos);
}

// What is not a PIDF document contributes nothing, and does not spoil the rest.
TEST(PidfTest, WhatIsNotPidfIsLeftOut) {
  const auto composed = pidf::compose("sip:alice@example.com", {"not xml at all", "<presence xmlns=\"urn:other\"><tuple id=\"x\"/></presence>",
                                                                document("<tuple id=\"cut\"><status>"), document(tuple("phone", "open"))});
  EXPECT_EQ(count(composed, "<tuple"), 1u);
  EXPECT_NE(composed.find(tuple("phone", "open")), std::string::npos);
}

// docs/design/presence.md: with nothing published, one tuple saying whether the AOR has a binding, and no contact.
TEST(PidfTest, WithNothingPublishedTheRegistrationsSaySo) {
  const auto open = pidf::registration("sip:alice@example.com", true);
  const auto closed = pidf::registration("sip:alice@example.com", false);
  EXPECT_NE(open.find("<basic>open</basic>"), std::string::npos);
  EXPECT_NE(closed.find("<basic>closed</basic>"), std::string::npos);
  EXPECT_NE(open.find("entity=\"sip:alice@example.com\""), std::string::npos);
  EXPECT_EQ(open.find("<contact"), std::string::npos);
  EXPECT_EQ(count(open, "<tuple"), 1u);
}

// The entity is escaped like any attribute.
TEST(PidfTest, TheEntityIsEscaped) {
  const auto composed = pidf::registration("sip:a&b@example.com", true);
  EXPECT_NE(composed.find("entity=\"sip:a&amp;b@example.com\""), std::string::npos);
}

// RFC 3863 4.1.4: each tuple's basic status is open or closed. The summary a console shows is open when any tuple is,
// as a buddy list shows someone reachable on any device.
TEST(PidfTest, ThePresentityIsOpenWhenAnyTupleIs) {
  EXPECT_EQ(pidf::basic(document(tuple("phone", "closed") + tuple("desk", "open"))), "open");
  EXPECT_EQ(pidf::basic(document(tuple("phone", "closed"))), "closed");
  EXPECT_EQ(pidf::basic(document("<note>nothing to say</note>")), "closed");

  // Under a prefix of the publisher's choosing, and with the whitespace XML allows.
  EXPECT_EQ(pidf::basic("<p:presence xmlns:p=\"urn:ietf:params:xml:ns:pidf\" entity=\"sip:a@example.com\"><p:tuple id=\"t\"><p:status><p:basic> open "
                        "</p:basic></p:status></p:tuple></p:presence>"),
            "open");
}
