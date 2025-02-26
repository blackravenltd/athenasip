//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "event_system.h"
#include "../global_io_context.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"

#include <boost/asio/post.hpp>
#include <boost/asio/io_context.hpp>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <string>
#include <functional>
#include <random>
#include <cstdint>

namespace athenasip::events {

class InternalEventSystem : public EventSystem, public std::enable_shared_from_this<InternalEventSystem> {
public:
    explicit InternalEventSystem(std::shared_ptr<athenasip::loggers::Logger> logger) : 
        _logger(std::make_shared<loggers::LoggerScoped>("internal_event_system", logger)),
        _io_context(detail::getGlobalIOContext())
    {}

    virtual ~InternalEventSystem() {}

    void start(std::function<void(bool)> callback) override {
        auto self = shared_from_this();

        _logger->info("Started");
        if(callback) {
            boost::asio::post(_io_context, [self, callback](){ callback(true); });
        }
    }
    void stop(std::function<void(bool)> callback) override {
        auto self = shared_from_this();

        // Kill all subscriptions
        unsubscribe_all(nullptr);

        if(callback) {
            boost::asio::post(_io_context, [self, callback](){ callback(true); });
        }
        _logger->info("Stopped");
    }

    void publish(std::string event_name, std::string message, std::function<void(bool)> callback) override {
        auto self = shared_from_this();

        _logger->debug("Publishing event: " + event_name + " with message: " + message);

        // Copy matching subscriptions under lock.
        std::unordered_set<std::shared_ptr<Subscription>> to_publish;
        {
            std::lock_guard<std::mutex> lock(_subscriptions_mutex);
            auto subs = _subscriptions.find(event_name);
            if (subs != _subscriptions.end()) {
                to_publish = subs->second;
            }
        }

        // Post each active subscription callback asynchronously.
        for (const auto& sub : to_publish) {
            boost::asio::post(_io_context, [this, self, event_name, message, sub]() {
                _logger->debug("Invoking subscription callback for event: " + event_name);
                sub->callback(event_name, message);
            });
        }

        if(callback) {
            // Post the publish callback.
            boost::asio::post(_io_context, [this, self, event_name, callback]() {
                _logger->debug("Invoking publish callback for event: " + event_name);
                callback(true);
            });
        }
    }

    std::shared_ptr<Subscription> subscribe(std::string event_name, std::function<void(std::string event_name, std::string message)> event_callback, std::function<void(bool)> callback) override {
         auto self = shared_from_this();

        _logger->debug("Subscribing to event: " + event_name);

        auto sub = std::make_shared<Subscription>(event_name, event_callback);

        {
            std::lock_guard<std::mutex> lock(_subscriptions_mutex);
            _subscriptions[event_name].insert(sub);
        }

        if(callback) {
            boost::asio::post(_io_context, [this, self, callback]() {
                _logger->debug("Invoking subscribe callback");
                callback(true);
            });
        }

        return sub;
    }

    void unsubscribe(std::shared_ptr<Subscription> subscription, std::function<void(bool)> callback) override {
        auto self = shared_from_this();

        {
            std::lock_guard<std::mutex> lock(_subscriptions_mutex);
            auto subs = _subscriptions.find(subscription->event_name);
            if(subs!=_subscriptions.end()) subs->second.erase(subscription);
        }
        if(callback) {
            boost::asio::post(_io_context, [this, self, callback]() {
                _logger->debug("Invoking unsubscribe callback");
                callback(true);
            });
        }
    }

    void unsubscribe_all(std::function<void(bool)> callback) override
    {
        auto self = shared_from_this();

        _logger->debug("Unsubscribing all subscriptions");
        {
            std::lock_guard<std::mutex> lock(_subscriptions_mutex);
            _subscriptions.clear();
        }
        if(callback) {
            boost::asio::post(_io_context, [this, self, callback]() {
                _logger->debug("Invoking unsubscribe_all callback");
                callback(true);
            });
        }
    }

private:
    std::shared_ptr<athenasip::loggers::Logger> _logger;
    boost::asio::io_context& _io_context;

    std::mutex _subscriptions_mutex;
    std::unordered_map<std::string, std::unordered_set<std::shared_ptr<Subscription>>> _subscriptions;
};

} // namespace athenasip::events
