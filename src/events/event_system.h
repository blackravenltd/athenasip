//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>
#include <functional>
#include <memory>

namespace athenasip::events {

class Subscription {
public:
    using EventCallbackFn = std::function<void(std::string, std::string)>;

    std::string event_name;
    EventCallbackFn callback;

    Subscription(std::string _event_name, EventCallbackFn _callback) : 
        event_name(_event_name), callback(_callback) {}
};

class EventSystem {
public:
    using CallbackCompleteFn = std::function<void(bool)>;

    virtual ~EventSystem() = default;

    virtual void start(CallbackCompleteFn callback) = 0;
    virtual void stop(CallbackCompleteFn callback) = 0;

    virtual void publish(std::string event_name, std::string message, CallbackCompleteFn callback) = 0;
    virtual std::shared_ptr<Subscription> subscribe(std::string event_name, Subscription::EventCallbackFn event_callback, CallbackCompleteFn) = 0;
    virtual void unsubscribe(std::shared_ptr<Subscription> subscription, CallbackCompleteFn) = 0;
    virtual void unsubscribe_all(std::function<void(bool)> callback) = 0;
};

} // namespace athenasip