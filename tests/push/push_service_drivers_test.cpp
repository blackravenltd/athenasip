//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "push/push_service_drivers.h"

#include <gtest/gtest.h>

#include <memory>

#include "../mocks/logger_mock.h"

using namespace athenasip;

// RFC 8599 sections 11 and 12: each driver is reached by, and named for, its pn-provider value.
TEST(PushServiceDriversTest, RegistersEachProviderUnderItsPnProviderValue) {
  auto logger = std::make_shared<MockLogger>();
  push::register_builtin_push_services(logger);

  auto webpush = push::PushService::create_driver(logger, "webpush://");
  ASSERT_TRUE(webpush);
  EXPECT_EQ(webpush->name(), "webpush");
  EXPECT_TRUE(std::dynamic_pointer_cast<push::WebpushPushService>(webpush));

  auto fcm = push::PushService::create_driver(logger, "fcm://");
  ASSERT_TRUE(fcm);
  EXPECT_EQ(fcm->name(), "fcm");
  EXPECT_TRUE(std::dynamic_pointer_cast<push::FcmPushService>(fcm));

  EXPECT_FALSE(push::PushService::create_driver(logger, "apns://"));
}
